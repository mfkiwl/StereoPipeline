// __BEGIN_LICENSE__
//  Copyright (c) 2009-2013, United States Government as represented by the
//  Administrator of the National Aeronautics and Space Administration. All
//  rights reserved.
//
//  The NGT platform is licensed under the Apache License, Version 2.0 (the
//  "License"); you may not use this file except in compliance with the
//  License. You may obtain a copy of the License at
//  http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
// __END_LICENSE__

// Keep here interest-point matching logic for StereoSession, as this logic
// can be slow to compile.

/// \file StereoSessionIp.cc
///
#include <vw/Core/Exception.h>
#include <vw/Core/Log.h>
#include <vw/Math/Vector.h>
#include <vw/Math/Functors.h>
#include <vw/Math/Geometry.h>
#include <vw/Image/PixelMask.h>
#include <vw/Image/PixelTypeInfo.h>
#include <vw/FileIO/DiskImageResource.h>
#include <vw/FileIO/DiskImageView.h>
#include <vw/Cartography/GeoReferenceUtils.h>

#include <asp/Sessions/StereoSession.h>
#include <asp/Core/InterestPointMatching.h> // Slow-to-compile ip header
#include <asp/Core/IpMatchingAlgs.h>        // Lightweight ip header
#include <asp/Core/AffineEpipolar.h>
#include <asp/Camera/RPCModel.h>

#include <boost/filesystem/operations.hpp>

#include <map>
#include <utility>
#include <string>
#include <ostream>
#include <limits>

using namespace vw;
using namespace vw::cartography;

namespace asp {

// A default IP matching implementation that derived classes can use
bool StereoSession::ip_matching(std::string const& input_file1,
                                std::string const& input_file2,
                                vw::Vector2 const& uncropped_image_size,
                                Vector6f    const& stats1,
                                Vector6f    const& stats2,
                                int ip_per_tile,
                                float nodata1, float nodata2,
                                vw::camera::CameraModel* cam1,
                                vw::camera::CameraModel* cam2,
                                std::string const& match_filename,
                                std::string const  left_ip_file,
                                std::string const  right_ip_file) {

  vw_out() << "\t--> Matching interest points in StereoSession.\n";

  // If we crop the images we must always create new matching files.
  // Otherwise, do not rebuild with externally provided match files,
  // or if a match file newer than the image and cameras is found in
  // the output directory.
  bool crop_left  = (stereo_settings().left_image_crop_win  != BBox2i(0, 0, 0, 0));
  bool crop_right = (stereo_settings().right_image_crop_win != BBox2i(0, 0, 0, 0));
  bool rebuild = (!is_latest_timestamp(match_filename, input_file1, input_file2,
                                       m_left_camera_file, m_right_camera_file));
  if (!crop_left && !crop_right &&
      (stereo_settings().force_reuse_match_files ||
       stereo_settings().clean_match_files_prefix != "" ||
       stereo_settings().match_files_prefix != ""))
    rebuild = false;
  if (crop_left || crop_right) 
    rebuild = true;
    
  if (boost::filesystem::exists(match_filename) && !rebuild) {
    vw_out() << "\t--> Using cached match file: " << match_filename << "\n";
    return true;
  }

  // If having to rebuild then wipe the old data
  if (boost::filesystem::exists(left_ip_file)) 
    boost::filesystem::remove(left_ip_file);
  if (boost::filesystem::exists(right_ip_file)) 
    boost::filesystem::remove(right_ip_file);
  if (boost::filesystem::exists(match_filename)) {
    vw_out() << "Removing old match file: " << match_filename << "\n";
    // It is hoped the logic before here was such that we will not
    // wipe external match files given by --match-files-prefix or
    // --clean-match-files-prefix.
    boost::filesystem::remove(match_filename);
  }
    
  // Create DiskImageResource objects. It is a little messy to make sure
  // it works with SPOT5 which will not work without the camera file
  // but the camera file does not match if the image is cropped. 
  // Ideally there would be a function to make this cleaner.
  boost::shared_ptr<DiskImageResource> rsrc1, rsrc2;
  if (input_file1 == m_left_image_file)
    rsrc1 = vw::DiskImageResourcePtr(m_left_image_file);
  else // Tiff input
    rsrc1 = vw::DiskImageResourcePtr(input_file1);
  if (input_file2 == m_right_image_file)
    rsrc2 = vw::DiskImageResourcePtr(m_right_image_file);
  else // Tiff input
    rsrc2 = vw::DiskImageResourcePtr(input_file2);

  DiskImageView<float> image1(rsrc1), image2(rsrc2);
  ImageViewRef<float> image1_norm = image1, image2_norm = image2;
  // Get normalized versions of the images for OpenCV based methods
  if ((stereo_settings().ip_matching_method != DETECT_IP_METHOD_INTEGRAL) &&
      (stats1[0] != stats1[1])) { // Don't normalize if no stats were provided
    vw_out() << "\t--> Normalizing images for IP detection using stats " << stats1 << "\n";
    bool do_not_exceed_min_max = false;
    asp::normalize_images(stereo_settings().force_use_entire_range,
                          stereo_settings().individually_normalize,
                          true, // Use percentile based stretch for ip matching
                          do_not_exceed_min_max,
                          stats1,      stats2,
                          image1_norm, image2_norm);
  }
    
  bool have_datum = this->have_datum();

  // If cameras are null then we cannot use them
  if (cam1 == NULL || cam2 == NULL) {
    have_datum = false;
  }
    
  bool inlier = false;
  if (have_datum) {
    // Run an IP matching function that takes the camera and datum info into account

    bool use_sphere_for_non_earth = true; // Assume Mars is a sphere
    cartography::Datum datum = this->get_datum(cam1, use_sphere_for_non_earth);

    // This is a bugfix. For RPC models, we must never intersect with
    // a datum whose height is outside of the domain of applicability
    // of the RPC model, as that can lead to very incorrect results.  
    const asp::RPCModel *rpc_cam
      = dynamic_cast<const asp::RPCModel*>(vw::camera::unadjusted_model(cam1));
    if (rpc_cam != NULL) {
      Vector3 lonlatheight_offset = rpc_cam->lonlatheight_offset();
      Vector3 lonlatheight_scale  = rpc_cam->lonlatheight_scale();
      double mid_ht = lonlatheight_offset[2];
      double min_ht = mid_ht - lonlatheight_scale[2];
      double max_ht = mid_ht + lonlatheight_scale[2];
      if (max_ht < 0) 
        vw_out() << "Warning: The RPC model maximum height is below the zero datum.\n";

      if (min_ht > 0) 
        vw_out() << "Warning: The RPC model minimum height is above the zero datum.\n";

      if (max_ht < 0 || min_ht > 0) {
        vw_out() << "RPC model min and max heights above datum: "
                 << min_ht << ' ' << max_ht << " meters.\n";
        vw_out() << "Adjusting the datum to compensate, for the purpose of alignment.\n";
        vw_out() << "The new datum height will be at " << mid_ht
                 << " meters relative to the previous one.\n";
        //vw_out() << "Old datum: " << datum << std::endl;
        datum.set_semi_major_axis(datum.semi_major_axis() + mid_ht);
        datum.set_semi_minor_axis(datum.semi_minor_axis() + mid_ht);
        //vw_out() << "New datum: " << datum << std::endl;
      }
    } // End RPC case

      // A smaller value here makes IP more unique, but also fewer. 
    double ip_uniqueness_thresh = stereo_settings().ip_uniqueness_thresh;

    // TODO: Improve calculation of epipolar parameter!
    // This computes a distance used for throwing out interest points.
    // - It has to be computed using the entire (not cropped) image size!
    // A larger value will keep more (but of lower quality) points.     
    double epipolar_threshold = norm_2(uncropped_image_size)/15;
    if (stereo_settings().epipolar_threshold > 0)
      epipolar_threshold = stereo_settings().epipolar_threshold;
    vw_out() << "\t    Using epipolar threshold = " << epipolar_threshold << std::endl;
    vw_out() << "\t    IP uniqueness threshold  = " << ip_uniqueness_thresh  << std::endl;
    vw_out() << "\t    Datum:                     " << datum << std::endl;
    if (stereo_settings().skip_rough_homography) {
      vw_out() << "\t    Skipping rough homography.\n";
      inlier = ip_matching_no_align(!supports_multi_threading(), cam1, cam2,
                                    image1_norm, image2_norm,
                                    ip_per_tile, datum,
                                    epipolar_threshold, ip_uniqueness_thresh,
                                    match_filename,
                                    left_ip_file, right_ip_file,
                                    nodata1, nodata2);
    } else {
      vw_out() << "\t    Using rough homography.\n";
      inlier = ip_matching_w_alignment(!supports_multi_threading(), cam1, cam2,
                                       image1_norm, image2_norm,
                                       ip_per_tile,
                                       datum, match_filename,
                                       epipolar_threshold, ip_uniqueness_thresh,
                                       left_ip_file, nodata1, nodata2);
    }
  } else { // Not nadir facing
    // Run a simpler purely image-based matching function
    double ip_inlier_factor = stereo_settings().ip_inlier_factor;
    int    inlier_threshold = round(ip_inlier_factor*150.0); // by default this is 10.

    // HACK: If the otherwise unused epipolar threshold is set, use it as
    //       the inlier threshold.
    if (stereo_settings().epipolar_threshold > 0)
      inlier_threshold = stereo_settings().epipolar_threshold;

    vw_out() << "\t    Not using a datum in interest point matching.\n";
    inlier = homography_ip_matching(image1_norm, image2_norm,
                                    ip_per_tile,
                                    inlier_threshold,
                                    match_filename,
                                    left_ip_file, right_ip_file,
                                    nodata1, nodata2);
  }
  if (!inlier) {
    boost::filesystem::remove(match_filename);
    vw_throw(IOErr() << "Unable to match left and right images.");
  }
  return inlier;
} // End function ip_matching()

// This logic is used in a handful of places  
std::string StereoSession::stereo_match_filename(std::string const& left_cropped_file,
                                                 std::string const& right_cropped_file,
                                                 std::string const& out_prefix) {
  
  // Define the file name containing IP match information.
  bool crop_left  = (stereo_settings().left_image_crop_win  != BBox2i(0, 0, 0, 0));
  bool crop_right = (stereo_settings().right_image_crop_win != BBox2i(0, 0, 0, 0));
  
  // See if can use an externally provided match file
  std::string match_filename;
  if (!crop_left && !crop_right)
    match_filename 
      = asp::match_filename(stereo_settings().clean_match_files_prefix,
                            stereo_settings().match_files_prefix,  
                            out_prefix, left_cropped_file, right_cropped_file);
  
  // If the user wants to use an external match file, it better exist
  bool external_matches = (!stereo_settings().clean_match_files_prefix.empty() ||
                           !stereo_settings().match_files_prefix.empty());
  if (external_matches && !boost::filesystem::exists(match_filename)) 
    vw_throw(ArgumentErr() << "Missing IP file: " << match_filename);
  
  // Fall back to creating one if no luck
  if (match_filename == "" || !boost::filesystem::exists(match_filename))
      match_filename = vw::ip::match_filename(out_prefix, left_cropped_file, right_cropped_file);
  
  return match_filename;
}
    
// Find ip matches and determine the alignment matrices
void StereoSession::determine_image_alignment(// Inputs
                                              std::string  const& out_prefix,
                                              std::string  const& left_cropped_file,
                                              std::string  const& right_cropped_file,
                                              std::string  const& left_uncropped_file,
                                              vw::Vector6f const& left_stats,
                                              vw::Vector6f const& right_stats,
                                              float left_nodata_value,
                                              float right_nodata_value,
                                              boost::shared_ptr<vw::camera::CameraModel>
                                              left_cam, 
                                              boost::shared_ptr<vw::camera::CameraModel>
                                              right_cam,
                                              bool adjust_left_image_size,
                                              // In-out
                                              vw::Matrix<double> & align_left_matrix,
                                              vw::Matrix<double> & align_right_matrix,
                                              vw::Vector2i & left_size,
                                              vw::Vector2i & right_size) {
  
  // Define the file name containing IP match information.
  std::string match_filename
    = this->stereo_match_filename(left_cropped_file, right_cropped_file, out_prefix);
  
  std::string left_ip_filename  = ip::ip_filename(out_prefix, left_cropped_file);
  std::string right_ip_filename = ip::ip_filename(out_prefix, right_cropped_file);
  
  // Detect matching interest points between the left and right input images.
  // The output is written directly to a file.
  DiskImageView<float> left_orig_image(left_uncropped_file);
  vw::Vector2 uncropped_left_image_size = bounding_box(left_orig_image).size();
  this->ip_matching(left_cropped_file, right_cropped_file,
                    uncropped_left_image_size,
                    left_stats, right_stats,
                    stereo_settings().ip_per_tile,
                    left_nodata_value, right_nodata_value,
                    left_cam.get(), right_cam.get(),
                    match_filename, left_ip_filename, right_ip_filename);
  
  // Load the interest points results from the file we just wrote
  std::vector<ip::InterestPoint> left_ip, right_ip;
  ip::read_binary_match_file(match_filename, left_ip, right_ip);
  
  // Compute the appropriate alignment matrix based on the input points
  if (stereo_settings().alignment_method == "homography") {
    left_size = homography_rectification(adjust_left_image_size,
                                         left_size, right_size, left_ip, right_ip,
                                         align_left_matrix, align_right_matrix);
    vw_out() << "\t--> Aligning right image to left using matrices:\n"
             << "\t      " << align_left_matrix  << "\n"
             << "\t      " << align_right_matrix << "\n";
  } else {
    // affineepipolar and local_epipolar
    bool crop_to_shared_area = true;
    left_size
      = affine_epipolar_rectification(left_size,         right_size,
                                      stereo_settings().global_alignment_threshold,
                                      stereo_settings().alignment_num_ransac_iterations,
                                      left_ip,           right_ip,
                                      crop_to_shared_area,
                                      align_left_matrix, align_right_matrix);
    vw_out() << "\t--> Aligning left and right images using affine matrices:\n"
             << "\t      " << submatrix(align_left_matrix, 0,0,2,3) << "\n"
             << "\t      " << submatrix(align_right_matrix,0,0,2,3) << "\n";
  }
  // Write out both computed matrices to disk
  write_matrix(out_prefix + "-align-L.exr", align_left_matrix);
  write_matrix(out_prefix + "-align-R.exr", align_right_matrix);
  
  // Because the images are now aligned they are the same size
  right_size = left_size;
}

} // End namespace asp
