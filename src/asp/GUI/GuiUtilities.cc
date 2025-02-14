// __BEGIN_LICENSE__
//  Copyright (c) 2006-2013, United States Government as represented by the
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

// TODO(oalexan1): This is very slow to compile. Need to figure out
// why. Could be the bundle adjustment logic. Or contouring, or
// shapefile logic, or DiskImagePyramid read from GuiUtilities.h.
// These may need to be moved to separate files.

#include <string>
#include <vector>
#include <QPolygon>
#include <QtGui>
#include <QtWidgets>
#include <ogrsf_frmts.h>

// For contours
#include <opencv2/imgproc.hpp>

#include <vw/Image/Algorithms.h>
#include <vw/Cartography/GeoTransform.h>
#include <vw/Cartography/Hillshade.h>
#include <vw/Core/RunOnce.h>
#include <vw/BundleAdjustment/ControlNetworkLoader.h>
#include <vw/InterestPoint/Matcher.h> // Needed for vw::ip::match_filename
#include <vw/Geometry/dPoly.h>
#include <vw/Cartography/shapeFile.h>
#include <vw/Core/Stopwatch.h>

#include <asp/GUI/GuiUtilities.h>
#include <asp/Core/StereoSettings.h>
#include <asp/Core/PointUtils.h>
#include <asp/GUI/chooseFilesDlg.h>

using namespace vw;
using namespace vw::gui;
using namespace vw::geometry;

namespace vw { namespace gui {

bool isPolyZeroDim(const QPolygon & pa){
  
  int numPts = pa.size();
  for (int s = 1; s < numPts; s++){
    if (pa[0] != pa[s]) return false;
  }
  
  return true;
}
  
bool getStringFromGui(QWidget * parent,
                      std::string title, std::string description,
                      std::string inputStr,
                      std::string & outputStr){ // output
  outputStr = "";

  bool ok = false;
  QString text = QInputDialog::getText(parent, title.c_str(), description.c_str(),
                                       QLineEdit::Normal, inputStr.c_str(),
                                       &ok);

  if (ok) outputStr = text.toStdString();

  return ok;
}

bool supplyOutputPrefixIfNeeded(QWidget * parent, std::string & output_prefix){

  if (output_prefix != "") return true;

  bool ans = getStringFromGui(parent,
                              "Enter the output prefix to use for the interest point match file.",
                              "Enter the output prefix to use for the interest point match file.",
                              "",
                              output_prefix);

  if (ans)
    vw::create_out_dir(output_prefix);

  return ans;
}

std::string fileDialog(std::string title, std::string start_folder){

  std::string fileName = QFileDialog::getOpenFileName(0,
                                      title.c_str(),
                                      start_folder.c_str()).toStdString();

  return fileName;
}

QRect bbox2qrect(BBox2 const& B){
  // Need some care here, an empty BBox2 can have its corners
  // as the largest double, which can cause overflow.
  if (B.empty()) 
    return QRect();
  return QRect(round(B.min().x()), round(B.min().y()),
               round(B.width()), round(B.height()));
}

bool write_hillshade(vw::GdalWriteOptions const& opt,
                     bool have_gui,
                     double azimuth, double elevation,
                     std::string const& input_file,
                     std::string      & output_file) {

  // Sanity check: Must have a georeference
  cartography::GeoReference georef;
  bool has_georef = vw::cartography::read_georeference(georef, input_file);
  if (!has_georef) {
    popUp("No georeference present in: " + input_file + ".");
    return false;
  }

  double scale       = 0.0;
  double blur_sigma  = std::numeric_limits<double>::quiet_NaN();
  double nodata_val  = std::numeric_limits<double>::quiet_NaN();
  vw::read_nodata_val(input_file, nodata_val);
  std::ostringstream oss;
  oss << "_hillshade_a" << azimuth << "_e" << elevation << ".tif"; 
  std::string suffix = oss.str();

  output_file = vw::mosaic::filename_from_suffix1(input_file, suffix);
  bool align_light_to_georef = false;
  try {
    DiskImageView<float> input(input_file);
    // TODO(oalexan1): Factor out repeated logic below.
    try {
      bool will_write = vw::mosaic::overwrite_if_no_good(input_file, output_file,
                                             input.cols(), input.rows());
      if (will_write) {
        vw_out() << "Writing: " << output_file << std::endl;
        vw::cartography::do_multitype_hillshade(input_file, output_file, azimuth, elevation, scale,
                                                nodata_val, blur_sigma, align_light_to_georef);
      }
    } catch(...) {
      // Failed to write, presumably because we have no write access.
      // Write the file in the current dir.
      vw_out() << "Failed to write: " << output_file << "\n";
      output_file = vw::mosaic::filename_from_suffix2(input_file, suffix);
      bool will_write = vw::mosaic::overwrite_if_no_good(input_file, output_file,
                                             input.cols(), input.rows());
      if (will_write) {
        vw_out() << "Writing: " << output_file << std::endl;
        vw::cartography::do_multitype_hillshade(input_file,  output_file,
                                                azimuth, elevation, scale,
                                                nodata_val, blur_sigma,
                                                align_light_to_georef);
      }
    }
  } catch (const Exception& e) {
    if (!have_gui) 
      vw_out() << e.what() << "\n";
    else
      popUp(e.what());
    return false;
  }
  
  return true;
}

// TODO(oalexan1): The 0.5 bias may be the wrong thing to do. Need to test
// more overlaying an image which is above threshold in a rectangular region
// and below it outside that region
void contour_image(DiskImagePyramidMultiChannel const& img,
                   vw::cartography::GeoReference const & georef,
                   double threshold,
                   std::vector<vw::geometry::dPoly> & polyVec) {
  
  std::vector<std::vector<cv::Point>> contours;
  std::vector<cv::Vec4i> hierarchy;

  // Create the OpenCV matrix. We will have issues for huge images.
  cv::Mat cv_img = cv::Mat::zeros(img.cols(), img.rows(), CV_8UC1);
  
  // Form the binary image. Values above threshold become 1, and less
  // than equal to the threshold become 0.
  long long int num_pixels_above_thresh = 0;
  for (int col = 0; col < img.cols(); col++) {
    for (int row = 0; row < img.rows(); row++) {
      uchar val = (std::max(img.get_value_as_double(col, row), threshold) - threshold > 0);
      cv_img.at<uchar>(col, row) = val;
      if (val > 0) 
        num_pixels_above_thresh++;
    }    
  }

  // Add the contour to the list of polygons to display
  polyVec.clear();
  polyVec.resize(1);
  vw::geometry::dPoly & poly = polyVec[0]; // alias

  if (num_pixels_above_thresh == 0) 
    return; // Return early, nothing to do
  
  // Find the contour
  cv::findContours(cv_img, contours, hierarchy, cv::RETR_TREE, cv::CHAIN_APPROX_SIMPLE);

  // Copy the polygon for export
  for (size_t k = 0; k < contours.size(); k++) {

    if (contours[k].empty()) 
      continue;

    // Copy from float to double
    std::vector<double> xv(contours[k].size()), yv(contours[k].size());
    for (size_t vIter = 0; vIter < contours[k].size(); vIter++) {

      // We would like the contour to go through the center of the
      // pixels not through their upper-left corners. Hence add 0.5.
      double bias = 0.5;
      
      // Note how we flip x and y, because in our GUI the first
      // coordinate is the column.
      Vector2 S(contours[k][vIter].y + bias, contours[k][vIter].x + bias);

      // The GUI expects the contours to be in georeferenced coordinates
      S = georef.pixel_to_point(S);
      
      xv[vIter] = S.x();
      yv[vIter] = S.y();
    }
    
    bool isPolyClosed = true;
    std::string color = "green";
    std::string layer = "0";
    poly.appendPolygon(contours[k].size(), &xv[0], &yv[0],  
                       isPolyClosed, color, layer);
  }
}

// This will tweak the georeference so that point_to_pixel() is the identity.
bool read_georef_from_shapefile(vw::cartography::GeoReference & georef,
                                std::string const& file){

  if (!asp::has_shp_extension(file))
    vw_throw(ArgumentErr() << "Expecting a shapefile as input, got: " << file << ".\n");
  
  bool has_georef;
  std::vector<vw::geometry::dPoly> polyVec;
  std::string poly_color;
  read_shapefile(file, poly_color, has_georef, georef, polyVec);
  
  return has_georef;
}
  
bool read_georef_from_image_or_shapefile(vw::cartography::GeoReference & georef,
                                         std::string const& file){

  if (asp::has_shp_extension(file)) 
    return read_georef_from_shapefile(georef, file);
  
  return vw::cartography::read_georeference(georef, file);
}

// Find the closest point in a given vector of polygons to a given point.
void findClosestPolyVertex(// inputs
                           double x0, double y0,
                           std::vector<vw::geometry::dPoly> const& polyVec,
                           // outputs
                           int & polyVecIndex,
                           int & polyIndexInCurrPoly,
                           int & vertIndexInCurrPoly,
                           double & minX, double & minY,
                           double & minDist) {
  
  polyVecIndex = -1; polyIndexInCurrPoly = -1; vertIndexInCurrPoly = -1;
  minX = x0; minY = y0; minDist = std::numeric_limits<double>::max();
  
  for (int s = 0; s < (int)polyVec.size(); s++){
    
    double minX0, minY0, minDist0;
    int polyIndex, vertIndex;
    polyVec[s].findClosestPolyVertex(// inputs
                                     x0, y0,
                                     // outputs
                                     polyIndex, vertIndex, minX0, minY0, minDist0);
    
    if (minDist0 <= minDist){
      polyVecIndex  = s;
      polyIndexInCurrPoly = polyIndex;
      vertIndexInCurrPoly = vertIndex;
      minDist       = minDist0;
      minX          = minX0;
      minY          = minY0;
    }

  }

  return;
}

// Find the closest edge in a given vector of polygons to a given point.
void findClosestPolyEdge(// inputs
                         double x0, double y0,
                         std::vector<vw::geometry::dPoly> const& polyVec,
                         // outputs
                         int & polyVecIndex,
                         int & polyIndexInCurrPoly,
                         int & vertIndexInCurrPoly,
                         double & minX, double & minY,
                         double & minDist){
  
  polyVecIndex = -1; polyIndexInCurrPoly = -1; vertIndexInCurrPoly = -1;
  minX = x0; minY = y0; minDist = std::numeric_limits<double>::max();
  
  for (int s = 0; s < (int)polyVec.size(); s++){
    
    double minX0, minY0, minDist0;
    int polyIndex, vertIndex;
    polyVec[s].findClosestPolyEdge(// inputs
                                   x0, y0,
                                   // outputs
                                   polyIndex, vertIndex, minX0, minY0, minDist0
                                   );
    
    if (minDist0 <= minDist){
      polyVecIndex  = s;
      polyIndexInCurrPoly = polyIndex;
      vertIndexInCurrPoly = vertIndex;
      minDist       = minDist0;
      minX          = minX0;
      minY          = minY0;
    }

  }

  return;
}

// Return true if the extension is .csv or .txt
bool hasCsv(std::string const& fileName) {
  std::string ext = get_extension(fileName);
  if (ext == ".csv" || ext == ".txt")
    return true;
  
  return false;
}

// Read datum from a csv file, such as a pointmap file saved by bundle_adjust.  
bool read_datum_from_csv(std::string const& file, vw::cartography::Datum & datum) {

  int count = 0;
  std::ifstream fh(file);
    std::string line;
    while (getline(fh, line, '\n') ) {

      // If the datum was not found early on, give up
      count++;
      if (count > 10)
        return false;

      if (vw::cartography::read_datum_from_str(line, datum)) 
        return true;
      
    }
  return false;
}

// Given one or more of --csv-format-str, --csv-proj4, and datum, extract
// the needed metatadata.
void read_csv_metadata(std::string              const& csv_file,
                       bool                            isPoly,
                       asp::CsvConv                  & csv_conv,
                       bool                          & has_pixel_vals,
                       bool                          & has_georef,
                       vw::cartography::GeoReference & georef) {

  if (!fs::exists(csv_file)) {
    popUp("Could not load file: " + csv_file);
    return;
  }
  
  if (asp::stereo_settings().csv_format_str == "") {
    // For the pointmap and match_offsets files the csv format is known, read it from
    // the file if not specified the user.  Same for anchor_points files written by
    // jitter_solve.
    if (csv_file.find("pointmap") != std::string::npos ||
        csv_file.find("match_offsets") != std::string::npos || 
        csv_file.find("anchor_points") != std::string::npos)
      asp::stereo_settings().csv_format_str = "1:lon, 2:lat, 4:height_above_datum";
    // For the diff.csv files produced by geodiff the csv format is known, read it from
    // the file if not specified the user.
    if (csv_file.find("-diff.csv") != std::string::npos)
      asp::stereo_settings().csv_format_str = "1:lon, 2:lat, 3:height_above_datum";
  }

  // For polygons, can assume that first coordinate is x and second is y
  if (isPoly && asp::stereo_settings().csv_format_str.empty())
    asp::stereo_settings().csv_format_str = "1:x, 2:y";
  
  if (asp::stereo_settings().csv_proj4 != "")
    vw_out() << "Using projection: " << asp::stereo_settings().csv_proj4 << "\n";

  try {
    int min_num_fields = 3;
    if (isPoly) 
      min_num_fields = 2; // only x and y coordinates may exist
    csv_conv.parse_csv_format(asp::stereo_settings().csv_format_str,
                              asp::stereo_settings().csv_proj4,
                              min_num_fields);
  } catch (...) {
    // Give a more specific error message
    popUp("Could not parse the csv format. Check or specify --csv-format.\n");
    exit(0); // This is fatal, can't do anything without the format
  }

  // For the x, y, z format we will just plot pixel x, pixel y, and value (z).
  // No georeference can be used.
  asp::CsvConv::CsvFormat fmt = csv_conv.get_format();
  if (fmt == asp::CsvConv::XYZ || fmt == asp::CsvConv::PIXEL_XYVAL)
    has_georef = false;
  else
    has_georef = true;

  has_pixel_vals = (fmt == asp::CsvConv::PIXEL_XYVAL);
  
  if (asp::stereo_settings().csv_format_str == "") {
    popUp("The option --csv-format-str must be specified.");
    exit(0); // this is fatal
    return;
  }
  vw_out() << "Using CSV format: " << asp::stereo_settings().csv_format_str << "\n";


  // Handle the datum
  bool has_datum = false;

  // For a pointmap file, anchor points, or a -diff.csv file, read the
  // datum from the file. The --csv-datum option, if set, will
  // override this.
  bool known_csv = (csv_file.find("pointmap") != std::string::npos ||
                    csv_file.find("anchor_points") != std::string::npos ||
                    csv_file.find("match_offsets") != std::string::npos ||
                    csv_file.find("-diff.csv") != std::string::npos);
  if (known_csv) {
    vw::cartography::Datum datum;
    if (read_datum_from_csv(csv_file, datum)) {
      georef.set_datum(datum);
      has_datum = true;
    }
  }
  
  // Parse the datum and populate the georef
  csv_conv.parse_georef(georef);
  if (asp::stereo_settings().csv_datum != "") {
    vw::cartography::Datum datum(asp::stereo_settings().csv_datum);
    georef.set_datum(datum);
    has_datum = true;
  }

  if (has_georef) {
    if (!has_datum) {
      popUp("Must specify --csv-datum.");
      return;
    }
  }

  if (has_datum)
    std::cout << "Using datum: " << georef.datum() << std::endl;
  
  return;
}

// Assemble the polygon structure
void formPoly(std::string              const& override_color,
              std::vector<int>         const& contiguous_blocks,
              std::vector<std::string> const& colors,
              std::vector<vw::Vector3> const& scattered_data, // input vertices
              std::vector<vw::geometry::dPoly> & polyVec) {

  // Wipe the output
  polyVec.clear();
  polyVec.resize(1);

  if (colors.size() != contiguous_blocks.size()) 
    vw::vw_throw(vw::ArgumentErr() << "There must be as many polygons as colors for them.\n");
  
  size_t vertexCount = 0;
  for (size_t polyIt = 0; polyIt < contiguous_blocks.size(); polyIt++) {
    
    std::vector<double> x, y;
    for (int vertexIt = 0; vertexIt < contiguous_blocks[polyIt]; vertexIt++) {
      
      if (vertexCount >= scattered_data.size())
        vw::vw_throw(vw::ArgumentErr() << "Book-keeping error in reading polygons.\n");
      
      x.push_back(scattered_data[vertexCount].x());
      y.push_back(scattered_data[vertexCount].y());
      vertexCount++;
    }
    
    std::string curr_color = colors[polyIt]; // use color from file if it exists
    
    // The command line color overrides what is in the file
    if (override_color != "default" && override_color != "")
      curr_color = override_color;
    
    bool isPolyClosed = true;
    std::string layer;
    polyVec[0].appendPolygon(x.size(),
                             vw::geometry::vecPtr(x), vw::geometry::vecPtr(y),
                             isPolyClosed, curr_color, layer);
  }
  
  if (vertexCount != scattered_data.size()) 
    vw::vw_throw(vw::ArgumentErr() << "The number of read vertices is not what is expected.\n");
  
  return;
}
  
void imageData::read(std::string const& name_in, vw::GdalWriteOptions const& opt,
                     DisplayMode display_mode,
                     std::map<std::string, std::string> const& properties) {
  
  vw_out() << "Reading: " << name_in << std::endl; 
  
  if (display_mode == REGULAR_VIEW)
    name = name_in;
  else if (display_mode == HILLSHADED_VIEW)
    hillshaded_name = name_in;
  else if (display_mode == THRESHOLDED_VIEW)
    thresholded_name = name_in;
  else if (display_mode == COLORIZED_VIEW)
    colorized_name = name_in;
  else
    vw::vw_throw(vw::ArgumentErr() << "Unknown display mode.\n");

  // TODO(oalexan1): There is no need to make the color a class member,
  // as it is already stored in individual polygons
  color = "default";
  style = "default";
  colormap = "binary-red-blue";
  colorize_image = false;

  m_opt = opt;
  m_display_mode = display_mode;

  // Properties passed on the command line; they take precedence
  for (auto it = properties.begin(); it != properties.end(); it++) {
    if (it->first == "color")
      color = it->second; // copy the poly/line/points color
    if (it->first == "style")
      style = it->second; // copy the style (poly, line, points)
    if (it->first == "colormap")
      colormap = it->second; // copy the colormap style (e.g., binary-red-blue)
    if (it->first == "colorize_image")
      colorize_image = atof(it->second.c_str()); 
  }

  std::string default_poly_color = "green"; // default, will be overwritten later
  
  if (asp::has_shp_extension(name_in)) {
    // Read a shape file
    std::string poly_color = default_poly_color;
    if (color != "default" && color != "") 
      poly_color = color;
    read_shapefile(name_in, poly_color, has_georef, georef, polyVec);

    double xll, yll, xur, yur;
    shapefile_bdbox(polyVec,
                    // Outputs
                    xll, yll, xur, yur);

    image_bbox.min() = Vector2(xll, yll);
    image_bbox.max() = Vector2(xur, yur);

    if (!has_georef)
      vw_out() << "The shapefile lacks a georeference.\n";
    
  } else if (vw::gui::hasCsv(name_in)) {

    bool isPoly = (style == "poly" || style == "fpoly" || style == "line");

    // Read CSV
    int numCols = asp::fileNumCols(name_in);
    bool has_pixel_vals = false; // may change later
    has_georef = true; // this may change later
    asp::CsvConv csv_conv;
    read_csv_metadata(name_in, isPoly, csv_conv, has_pixel_vals, has_georef, georef);

    std::vector<int> contiguous_blocks;
    std::vector<std::string> colors;
    colors.push_back(default_poly_color); // to provide a default color for the reader

    // Read the file
    std::list<asp::CsvConv::CsvRecord> pos_records;
    if (!isPoly)
      csv_conv.read_csv_file(name_in, pos_records);
    else
      csv_conv.read_poly_file(name_in, pos_records, contiguous_blocks, colors);
    
    scattered_data.clear();
    vw::BBox3 bounds;
    for (auto iter = pos_records.begin(); iter != pos_records.end(); iter++) {
      Vector3 val = csv_conv.sort_parsed_vector3(*iter);

      // For pixel values the y axis will go down
      if (has_pixel_vals)
        val[1] *= -1.0;
      
      scattered_data.push_back(val);
      bounds.grow(val);
    }
    image_bbox.min() = subvector(bounds.min(), 0, 2);
    image_bbox.max() = subvector(bounds.max(), 0, 2);
    val_range[0] = bounds.min()[2];
    val_range[1] = bounds.max()[2];

    if (isPoly) {
      formPoly(color, contiguous_blocks, colors, scattered_data, polyVec);
      scattered_data.clear(); // the data is now in the poly structure
    }
    
  }else{
    // Read an image
    int top_image_max_pix = 1000*1000;
    int subsample = 4;
    has_georef = vw::cartography::read_georeference(georef, name_in);

    if (display_mode == REGULAR_VIEW) {
      img = DiskImagePyramidMultiChannel(name, m_opt, top_image_max_pix, subsample);
      image_bbox = BBox2(0, 0, img.cols(), img.rows());
    } else if (display_mode == HILLSHADED_VIEW) {
      hillshaded_img = DiskImagePyramidMultiChannel(hillshaded_name, m_opt,
                                                    top_image_max_pix, subsample);
      image_bbox = BBox2(0, 0, hillshaded_img.cols(), hillshaded_img.rows());
    } else if (display_mode == THRESHOLDED_VIEW) {
      thresholded_img = DiskImagePyramidMultiChannel(thresholded_name, m_opt,
                                                     top_image_max_pix, subsample);
      image_bbox = BBox2(0, 0, thresholded_img.cols(), thresholded_img.rows());
    } else if (display_mode == COLORIZED_VIEW) {
      colorized_img = DiskImagePyramidMultiChannel(colorized_name, m_opt,
                                                     top_image_max_pix, subsample);
      image_bbox = BBox2(0, 0, colorized_img.cols(), colorized_img.rows());
    }
  }
}

bool imageData::isPoly() const {
  return (asp::has_shp_extension(name) ||
          (vw::gui::hasCsv(name) && (style == "poly" || style == "fpoly" || style == "line")));
}
  
bool imageData::isCsv() const {
  return vw::gui::hasCsv(name) && !isPoly();
}
  
vw::Vector2 QPoint2Vec(QPoint const& qpt) {
  return vw::Vector2(qpt.x(), qpt.y());
}

QPoint Vec2QPoint(vw::Vector2 const& V) {
  return QPoint(round(V.x()), round(V.y()));
}

void PointList::push_back(std::list<vw::Vector2> pts) {
  std::list<vw::Vector2>::iterator iter  = pts.begin();
  while (iter != pts.end()) {
    m_points.push_back(*iter);
    ++iter;
  }
}


//========================
// Functions for MatchList

void MatchList::throwIfNoPoint(size_t image, size_t point) const {
  if ((image >= m_matches.size()) || (point >= m_matches[image].size()))
    vw_throw(ArgumentErr() << "IP " << image << ", " << point << " does not exist!\n");
}

void MatchList::resize(size_t num_images) {
  m_matches.clear();
  m_valid_matches.clear();
  m_matches.resize(num_images);
  m_valid_matches.resize(num_images);
}

bool MatchList::addPoint(size_t image, vw::ip::InterestPoint const &pt, bool valid) {

  if (image >= m_matches.size())
    return false;

  // We will start with an interest point in the left-most image,
  // and add matches to it in the other images.
  // At any time, an image to the left must have no fewer ip than
  // images on the right. Upon saving, all images must
  // have the same number of interest points.
  size_t curr_pts = m_matches[image].size(); // # Pts from current image
  bool is_good = true;
  for (size_t i = 0; i < image; i++) { // Look through lower-id images
    if (m_matches[i].size() < curr_pts+1) {
      is_good = false;
    }
  }
  // Check all higher-id images, they should have the same # Pts as this one.
  for (size_t i = image+1; i < m_matches.size(); i++) {
    if (m_matches[i].size() > curr_pts) {
      is_good = false;
    }
  }

  if (!is_good)
    return false;

  m_matches[image].push_back(pt);
  m_valid_matches[image].push_back(true);
  return true;
}

size_t MatchList::getNumImages() const {
  return m_matches.size();
}

size_t MatchList::getNumPoints(size_t image) const {
  if (m_matches.empty())
    return 0;
  return m_matches[image].size();
}

vw::ip::InterestPoint const& MatchList::getPoint(size_t image, size_t point) const {
  throwIfNoPoint(image, point);
  return m_matches[image][point];
}

vw::Vector2 MatchList::getPointCoord(size_t image, size_t point) const {
  throwIfNoPoint(image, point);
  return vw::Vector2(m_matches[image][point].x, m_matches[image][point].y);
}

bool MatchList::pointExists(size_t image, size_t point) const {
  return ((image < m_matches.size()) && (point < m_matches[image].size()));
}

bool MatchList::isPointValid(size_t image, size_t point) const {
  throwIfNoPoint(image, point);
  return m_valid_matches[image][point];
}

void MatchList::setPointValid(size_t image, size_t point, bool newValue) {
  throwIfNoPoint(image, point);
  m_valid_matches[image][point] = newValue;
}

void MatchList::setPointPosition(size_t image, size_t point, float x, float y) {
  throwIfNoPoint(image, point);
  m_matches[image][point].x = x;
  m_matches[image][point].y = y;
}

int MatchList::findNearestMatchPoint(size_t image, vw::Vector2 P, double distLimit) const {
  if (image >= m_matches.size())
    return -1;

  double min_dist  = std::numeric_limits<double>::max();
  if (distLimit > 0)
    min_dist = distLimit;
  int    min_index = -1;
  std::vector<vw::ip::InterestPoint> const& ip = m_matches[image]; // alias
  for (size_t ip_iter = 0; ip_iter < ip.size(); ip_iter++) {
    Vector2 Q(ip[ip_iter].x, ip[ip_iter].y);
    double curr_dist = norm_2(Q-P);
    if (curr_dist < min_dist) {
      min_dist  = curr_dist;
      min_index = ip_iter;
    }
  }
  return min_index;
}

void MatchList::deletePointsForImage(size_t image) {
  if (image >= m_matches.size() )
    vw_throw(ArgumentErr() << "Image " << image << " does not exist!\n");

  m_matches.erase      (m_matches.begin()       + image);
  m_valid_matches.erase(m_valid_matches.begin() + image);
}

bool MatchList::deletePointAcrossImages(size_t point) {

  // Sanity checks
  if (point >= getNumPoints()){
    popUp("Requested point for deletion does not exist!");
    return false;
  }
  for (size_t i = 0; i < m_matches.size(); i++) {
    if (m_matches[0].size() != m_matches[i].size()) {
      popUp("Cannot delete matches. Must have the same number of matches in each image.");
      return false;
    }
  }

  for (size_t vec_iter = 0; vec_iter < m_matches.size(); vec_iter++) {
    m_matches[vec_iter].erase(m_matches[vec_iter].begin() + point);
    m_valid_matches[vec_iter].erase(m_valid_matches[vec_iter].begin() + point);
  }
  return true;
}

bool MatchList::allPointsValid() const {
  if (m_valid_matches.size() != m_matches.size())
    vw_throw(LogicErr() << "Valid matches out of sync with matches!\n");
  for (size_t i = 0; i < m_matches.size(); i++) {
    if (m_matches[0].size() != m_matches[i].size())
      return false;
    for (size_t j=0; j<m_valid_matches[i].size(); ++j) {
      if (!m_valid_matches[i][j])
        return false;
    }
  } // End loop through images.
  return true;
}

bool MatchList::loadPointsFromGCPs(std::string const gcpPath,
                                   std::vector<std::string> const& imageNames) {
  using namespace vw::ba;

  if (getNumPoints() > 0) // Can't double-load points!
    return false;

  const size_t num_images = imageNames.size();
  resize(num_images);

  ControlNetwork cnet("gcp");
  cnet.get_image_list() = imageNames;
  std::vector<std::string> gcp_files;
  gcp_files.push_back(gcpPath);
  vw::cartography::Datum datum; // the actual datum does not matter here
  try {
    add_ground_control_points(cnet, gcp_files, datum);
  }catch(...){
    // Do not complain if the GCP file does not exist. Maybe we want to create it.
    return true;
  }
  
  CameraRelationNetwork<JFeature> crn;
  crn.from_cnet(cnet);

  typedef CameraNode<JFeature>::iterator crn_iter;
  if (crn.size() != num_images && crn.size() != 0) {
    popUp("The number of images in the control network does not agree with the number of images to view.");
    return false;
  }

  // Load in all of the points
  for ( size_t icam = 0; icam < crn.size(); icam++ ) {
    for ( crn_iter fiter = crn[icam].begin(); fiter != crn[icam].end(); fiter++ ){
      Vector2 observation = (**fiter).m_location;
      vw::ip::InterestPoint ip(observation.x(), observation.y());
      m_matches[icam].push_back(ip);
      m_valid_matches[icam].push_back(true);
    }
  }

  // If any of the sizes do not match, reset everything!
  for ( size_t icam = 0; icam < crn.size(); icam++ ) {
    if (m_matches[0].size() != m_matches[icam].size()) {
      popUp("Each GCP must be represented as a pixel in each image.");
      resize(num_images);
      return false;
    }
  }

  return true;
}

bool MatchList::loadPointsFromVwip(std::vector<std::string> const& vwipFiles,
                                   std::vector<std::string> const& imageNames){

  using namespace vw::ba;

  if (getNumPoints() > 0) // Can't double-load points!
    return false;

  const size_t num_images = imageNames.size();
  resize(num_images);

  // Load in all of the points
  for (size_t i = 0; i < num_images; ++i) {
    //std::vector<InterestPoint> ip;
    m_matches[i] = vw::ip::read_binary_ip_file(vwipFiles[i]);
    // Keep the valid matches synced up
    size_t num_pts = m_matches[i].size();
    m_valid_matches[i].resize(num_pts);
    for (size_t j=0; j<num_pts; ++j)
       m_valid_matches[i][j] = true;
  }
  
  return true;
}

void MatchList::setIpValid(size_t image) {
  if (image >= getNumImages())
    return;
  const size_t num_ip = m_matches[image].size();
  m_valid_matches[image].resize(num_ip);
  for (size_t i=0; i<num_ip; ++i)
    m_valid_matches[image][i] = true;
}

bool MatchList::loadPointsFromMatchFiles(std::vector<std::string> const& matchFiles,
                                         std::vector<size_t     > const& leftIndices) {

  // Count IP as in the same location if x and y are at least this close.
  const float ALLOWED_POS_DIFF = 0.5;

  // Can't double-load points!
  if ((getNumPoints() > 0) || (matchFiles.empty()))
    return false;

  const size_t num_images = matchFiles.size() + 1;
  // Make sure we have the right number of match files
  if ((matchFiles.size() != leftIndices.size()))
    return false;

  resize(num_images);

  // Loop through all of the matches
  size_t num_ip = 0;
  for (size_t i = 1; i < num_images; i++) {
    std::string match_file = matchFiles [i-1];
    size_t      j         = leftIndices[i-1];

    // Initialize all matches as invalid
    m_matches      [i].resize(num_ip);
    m_valid_matches[i].resize(num_ip);
    for (size_t v=0; v<num_ip; ++v) {
      m_matches      [i][v].x = v*10;  // TODO: Better way to spread these IP?
      m_matches      [i][v].y = v*10;
      m_valid_matches[i][v] = false;
    }

    std::vector<vw::ip::InterestPoint> left, right;
    try {
      vw_out() << "Reading binary match file: " << match_file << std::endl;
      ip::read_binary_match_file(match_file, left, right);
    }catch(...){
      vw_out() << "IP load failed, leaving default invalid IP\n";
      continue;
    }

    if (i == 1) { // The first case is easy
      m_matches[0] = left;
      m_matches[1] = right;
      setIpValid(0);
      setIpValid(1);
      num_ip = left.size(); // The first image sets the number of IP
      continue;
    }

    // For other cases, we need to isolate the same IP in the left image.
    // Loop through the ip in the "left" image
    size_t count = 0;
    for (size_t pnew=0; pnew<left.size(); ++pnew) {

      // Look through the ip we already have for that image
      //  and see if any of them are at the same location
      for (size_t pold=0; pold<num_ip; ++pold) {

        float dx = fabs(left[pnew].x - m_matches[j][pold].x);
        float dy = fabs(left[pnew].y - m_matches[j][pold].y);
        if ((dx < ALLOWED_POS_DIFF) && (dy < ALLOWED_POS_DIFF))
        {
          // If we found a match, record it and move on to the next point.
          // - Note that we match left[] but we record right[]
          m_matches      [i][pold] = right[pnew];
          m_valid_matches[i][pold] = true;
          ++count;
          break;
        }
      } // End loop through m_matches[j]
      
      if (count == num_ip)
        break; // This means we matched all of the IP in the existing image!
      
    }  // End loop through left
    // Any points that did not match are left with their original value.
  }
  return true;
}

bool MatchList::savePointsToDisk(std::string const& prefix,
                                 std::vector<std::string> const& imageNames,
                                 std::string const& match_file) const {
  if (!allPointsValid() || (imageNames.size() != m_matches.size())) {
    popUp("Cannot write match files, not all points are valid.");
    return false;
  }

  const size_t num_image_files = imageNames.size();

  bool success = true;
  for (size_t i = 0; i < num_image_files; i++) {

    // Save both i to j matches and j to i matches if there are more than two images.
    // This is useful for SfS, though it is a bit of a hack.
    size_t beg = i + 1;
    if (num_image_files > 2) 
      beg = 0;

    for (size_t j = beg; j < num_image_files; j++) {

      if (i == j)
        continue; // don't save i <-> i matches

      std::string output_path = vw::ip::match_filename(prefix, imageNames[i], imageNames[j]);
      if ((num_image_files == 2) && (match_file != ""))
        output_path = match_file;
      try {
        vw_out() << "Writing: " << output_path << std::endl;
        ip::write_binary_match_file(output_path, m_matches[i], m_matches[j]);
      }catch(...){
        popUp("Failed to save match file: " + output_path);
        success = false;
      }
    }
  }
  return success;
}

// See if we are in the mode where the images are displayed side-by-side with a
// dialog to choose which ones to display.
bool sideBySideWithDialog() {
  return (asp::stereo_settings().pairwise_matches         ||
          asp::stereo_settings().pairwise_clean_matches);
}

// Turn off any such side-by-side logic
void setNoSideBySideWithDialog() {
  asp::stereo_settings().pairwise_matches         = false;
  asp::stereo_settings().pairwise_clean_matches   = false;
}

  
}} // namespace vw::gui
