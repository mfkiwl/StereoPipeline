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

/// \file MainWidget.cc
///
///
// TODO(oalexan1): Each layer must have just a dPoly, rather
// than a vector of them.
/// TODO: Test with empty images and images having just one pixel.

#include <string>
#include <vector>
#include <QtGui>
#include <QtWidgets>

#include <vw/Math/EulerAngles.h>
#include <vw/Image/Algorithms.h>
#include <vw/Core/RunOnce.h>
#include <vw/Cartography/GeoReferenceUtils.h>
#include <vw/Cartography/GeoTransform.h>
#include <vw/Cartography/shapeFile.h>
#include <vw/Core/Stopwatch.h>

#include <asp/GUI/MainWidget.h>
#include <asp/Core/StereoSettings.h>

using namespace vw;
using namespace vw::cartography;

namespace vw { namespace gui {

  // --------------------------------------------------------------
  //               MainWidget Public Methods
  // --------------------------------------------------------------

  int MainWidget::getTransformImageIndex() const {
    // TODO: Is there a better way to determine this?
    size_t trans_image_id = m_image_id;
    if (trans_image_id >= m_world2image_geotransforms.size())
      trans_image_id = 0;
    return trans_image_id;
  }

  // Convert a position in the world coordinate system to a pixel
  // position as seen on screen (the screen origin is the
  // visible upper-left corner of the widget).
  Vector2 MainWidget::world2screen(Vector2 const p) const{

    double x = m_window_width*((p.x() - m_current_view.min().x())
                               /m_current_view.width());
    double y = m_window_height*((p.y() - m_current_view.min().y())
                                /m_current_view.height());

    // Create an empty border margin, to make it easier to zoom
    // by allowing the zoom window to slightly exceed the visible image
    // area (that inability was such a nuisance).
    x = m_border_factor*(x - m_window_width /2.0) + m_window_width /2.0;
    y = m_border_factor*(y - m_window_height/2.0) + m_window_height/2.0;

    return vw::Vector2(x, y);
  }

  // Convert a pixel on the screen to world coordinates.
  // See world2image() for the definition.
  Vector2 MainWidget::screen2world(Vector2 const p) const{

    // First undo the empty border margin
    double x = p.x(), y = p.y();
    x = (x - m_window_width /2.0)/m_border_factor + m_window_width /2.0;
    y = (y - m_window_height/2.0)/m_border_factor + m_window_height/2.0;

    // Scale to world coordinates
    x = m_current_view.min().x() + (m_current_view.width () * x / m_window_width );
    y = m_current_view.min().y() + (m_current_view.height() * y / m_window_height);

    return vw::Vector2(x, y);
  }

  BBox2 MainWidget::screen2world(BBox2 const& R) const{
    if (R.empty()) return R;
    Vector2 A = screen2world(R.min());
    Vector2 B = screen2world(R.max());
    return BBox2(A, B);
  }

  BBox2 MainWidget::world2screen(BBox2 const& R) const {
    if (R.empty()) return R;
    Vector2 A = world2screen(R.min());
    Vector2 B = world2screen(R.max());
    return BBox2(A, B);
  }

  // TODO(oalexan1): Rename world2image to world2pixel.
  // Then also have world2projpt, for both a point and a box. 
  
  // If we use georef, the world is in projected point units of the
  // first image, with y replaced with -y, to keep the y axis downward,
  // for consistency with how images are plotted.  Convert a world box
  // to a pixel box for the given image.
  Vector2 MainWidget::world2image(Vector2 const P, int imageIndex) const{
    if (!m_use_georef)
      return P;

    // There is no pixel concept in that case
    if (m_images[imageIndex].isPoly())
      return flip_in_y(P);
      
    return m_world2image_geotransforms[imageIndex].point_to_pixel(flip_in_y(P));
  }

  BBox2 MainWidget::world2image(BBox2 const& R, int imageIndex) const{

    if (R.empty()) 
      return R;
    if (m_images.empty()) 
      return R;
    if (!m_use_georef)
      return R;

    // There is no pixel concept in that case
    if (m_images[imageIndex].isPoly())
      return m_world2image_geotransforms[imageIndex].point_to_point_bbox(flip_in_y(R));

    return m_world2image_geotransforms[imageIndex].point_to_pixel_bbox(flip_in_y(R));
  }

  // The reverse of world2image()
  Vector2 MainWidget::image2world(Vector2 const P, int imageIndex) const{

    if (!m_use_georef)
      return P;

    // There is no pixel concept in that case
    if (m_images[imageIndex].isPoly())
      return flip_in_y(P);

    return flip_in_y(m_image2world_geotransforms[imageIndex].pixel_to_point(P));
  }

  // The reverse of world2image()
  BBox2 MainWidget::image2world(BBox2 const& R, int imageIndex) const{

    if (R.empty()) return R;
    if (m_images.empty()) return R;

    if (!m_use_georef)
      return R;

    // Consider the case when the current layer is a polygon.
    // TODO(oalexan1): What if a layer has both an image and a polygon?
    
    if (m_images[imageIndex].isPoly()) 
      return flip_in_y(m_image2world_geotransforms[imageIndex].point_to_point_bbox(R));

    return flip_in_y(m_image2world_geotransforms[imageIndex].pixel_to_point_bbox(R));
  }

  // Convert from world coordinates to projected coordinates in given geospatial
  // projection
  Vector2 MainWidget::world2projpoint(Vector2 P, int imageIndex) const{
    if (!m_use_georef)
      return P;
    return m_world2image_geotransforms[imageIndex].point_to_point(flip_in_y(P)); 
  }
  
  // The reverse of world2projpoint
  Vector2 MainWidget::projpoint2world(Vector2 P, int imageIndex) const{
    if (!m_use_georef)
      return P;
    return flip_in_y(m_image2world_geotransforms[imageIndex].point_to_point(P));
  }

  MainWidget::MainWidget(QWidget *parent,
                         vw::cartography::GdalWriteOptions const& opt,
                         int image_id,
                         std::string & output_prefix,
                         std::vector<std::string> const& image_files,
                         std::string const& base_image_file,
                         MatchList & matches,
                         int &editMatchPointVecIndex,
                         chooseFilesDlg * chooseFiles,
                         bool use_georef, std::vector<bool> const& hillshade, bool view_matches,
                         bool zoom_all_to_same_region, bool & allowMultipleSelections)
    : QWidget(parent), m_opt(opt), m_chooseFilesDlg(chooseFiles),
      m_image_id(image_id), m_output_prefix(output_prefix),
      m_image_files(image_files), 
      m_matchlist(matches),  m_editMatchPointVecIndex(editMatchPointVecIndex),
      m_use_georef(use_georef),
      m_view_matches(view_matches), m_zoom_all_to_same_region(zoom_all_to_same_region),
      m_allowMultipleSelections(allowMultipleSelections), m_can_emit_zoom_all_signal(false),
      m_polyEditMode(false), m_polyLayerIndex(0),
      m_pixelTol(6), m_backgroundColor(QColor("black")),
      m_lineWidth(1), m_polyColor("green"), m_editingMatches(false) {

    installEventFilter(this);

    m_firstPaintEvent = true;
    m_emptyRubberBand = QRect(0, 0, 0, 0);
    m_rubberBand      = m_emptyRubberBand;
    m_cropWinMode     = false;
    m_profileMode     = false;
    m_profilePlot     = NULL;
    m_world_box       = BBox2();
    
    m_mousePrsX = 0;
    m_mousePrsY = 0;

    m_border_factor = 0.95;

    // Set mouse tracking
    this->setMouseTracking(true);

    // Set the size policy that the widget can grow or shrink and still
    // be useful.
    this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    this->setFocusPolicy(Qt::ClickFocus);

    // Read the images. Find the box that will contain all of them.
    // If we use georef, that box is in projected point units
    // of the first image.
    // Also set up the image GeoReference transforms for each image
    // in both directions.
    int num_images = image_files.size();
    m_images.resize(num_images);
    m_filesOrder.resize(num_images);
    m_world2image_geotransforms.resize(num_images);
    m_image2world_geotransforms.resize(num_images);
    for (int i = 0; i < num_images; i++) {
      m_images[i].read(image_files[i], m_opt);

      if (m_use_georef && !m_images[i].has_georef){
        popUp("No georeference present in: " + m_images[i].name + ".");
        vw_throw(ArgumentErr() << "Missing georeference.\n");
      }
          
      // Read the base image, if different from the current image.
      // When using georeferenced images, the base image projection
      // (flipped in y) becomes the world coordinates.
      if (i == 0){
        if (image_files[i] == base_image_file) {
          m_base_image = m_images[i];
        }else{
          m_base_image.read(base_image_file, m_opt);
        }
      }
      
      // Make sure we set these up before the image2world call below!
      m_world2image_geotransforms[i] = GeoTransform(m_base_image.georef, m_images[i].georef);
      m_image2world_geotransforms[i] = GeoTransform(m_images[i].georef, m_base_image.georef);
      
      m_filesOrder[i] = i; // start by keeping the order of files being read

      // Grow the world box to fit all the images
      BBox2 B = MainWidget::image2world(m_images[i].image_bbox, i);
      m_world_box.grow(B);

      // The first existing vector layer becomes the one we draw on.
      // Otherwise we keep m_polyLayerIndex at 0 so we store any new
      // polygons in m_images[0].
      if (m_images[i].isPoly() && m_polyLayerIndex == 0)
        m_polyLayerIndex = i;
      
    } // end iterating over the images

    // Each image can be hillshaded independently of the other ones
    m_hillshade_mode      = hillshade;
    m_hillshade_azimuth   = asp::stereo_settings().hillshade_azimuth;
    m_hillshade_elevation = asp::stereo_settings().hillshade_elevation;
    
    // Image threshold
    m_thresh = -std::numeric_limits<double>::max();
    m_thresh_calc_mode = false;
    m_thresh_view_mode = false;

    // To do: Warn the user if some images have georef while others don't.

    // Choose which files to hide/show in the GUI
    if (m_chooseFilesDlg){
      
      m_chooseFilesDlg->chooseFiles(m_images, asp::stereo_settings().hide_all);

      // Make list of all the unchecked files.
      // TODO: It is poor design that we keep in both the table and
      // in m_filesToHide the state of which files to hide and these
      // need to be synched up.
      updateFilesToHide();
      
      // When the user clicks on a table entry, say by modifying a 
      // checkbox, update the display.
      QObject::connect(m_chooseFilesDlg->getFilesTable(),
                       SIGNAL(cellClicked(int, int)),
                       this,
                       SLOT(showFilesChosenByUser(int, int)));
      
      // When the user clicks on the table header on top to toggle all on/off
      QObject::connect(m_chooseFilesDlg->getFilesTable()->horizontalHeader(),
                       SIGNAL(sectionClicked(int)), this, SLOT(toggleAllOnOff()));
      
      m_chooseFilesDlg->getFilesTable()->setContextMenuPolicy(Qt::CustomContextMenu);
      QObject::connect(m_chooseFilesDlg->getFilesTable(),
                       SIGNAL(customContextMenuRequested(QPoint)),
                       this, SLOT(customMenuRequested(QPoint)));
      
    }

    // Right-click context menu
    m_ContextMenu = new QMenu();
    //setContextMenuPolicy(Qt::CustomContextMenu);

    // Polygon editing mode, they will be visible only when editing happens
    m_insertVertex   = m_ContextMenu->addAction("Insert vertex");
    m_deleteVertex   = m_ContextMenu->addAction("Delete vertex");
    m_deleteVertices = m_ContextMenu->addAction("Delete vertices in selected region");
    m_moveVertex     = m_ContextMenu->addAction("Move vertices");
    m_moveVertex->setCheckable(true);
    m_moveVertex->setChecked(false);

    m_showPolysFilled = m_ContextMenu->addAction("Show polygons filled");
    m_showPolysFilled->setCheckable(true);
    m_showPolysFilled->setChecked(false);

    m_showIndices = m_ContextMenu->addAction("Show vertex indices");
    m_showIndices->setCheckable(true);
    m_showIndices->setChecked(false);

    m_mergePolys = m_ContextMenu->addAction("Merge polygons");
    
    m_saveVectorLayer = m_ContextMenu->addAction("Save vector layer as shape file");
      
    // Other options
    m_addMatchPoint      = m_ContextMenu->addAction("Add match point");
    m_deleteMatchPoint   = m_ContextMenu->addAction("Delete match point");
    m_moveMatchPoint     = m_ContextMenu->addAction("Move match point");
    m_moveMatchPoint->setCheckable(true);
    m_moveMatchPoint->setChecked(false);
    m_toggleHillshade    = m_ContextMenu->addAction("Toggle hillshaded display");
    m_setHillshadeParams = m_ContextMenu->addAction("View/set hillshade azimuth and elevation");
    m_saveScreenshot     = m_ContextMenu->addAction("Save screenshot");
    m_setThreshold       = m_ContextMenu->addAction("View/set threshold");
    m_allowMultipleSelections_action = m_ContextMenu->addAction("Allow multiple selected regions");
    m_allowMultipleSelections_action->setCheckable(true);
    m_allowMultipleSelections_action->setChecked(m_allowMultipleSelections);
    m_deleteSelection = m_ContextMenu->addAction("Delete selected regions around this point");
    m_hideImagesNotInRegion = m_ContextMenu->addAction("Hide images not intersecting selected region");

    connect(m_addMatchPoint,         SIGNAL(triggered()), this, SLOT(addMatchPoint()));
    connect(m_deleteMatchPoint,      SIGNAL(triggered()), this, SLOT(deleteMatchPoint()));
    connect(m_toggleHillshade,       SIGNAL(triggered()), this, SLOT(toggleHillshade()));
    connect(m_setHillshadeParams,    SIGNAL(triggered()), this, SLOT(setHillshadeParams()));
    connect(m_setThreshold,          SIGNAL(triggered()), this, SLOT(setThreshold()));
    connect(m_saveScreenshot,        SIGNAL(triggered()), this, SLOT(saveScreenshot()));
    connect(m_allowMultipleSelections_action, SIGNAL(triggered()), this,
                                                                SLOT(allowMultipleSelections()));
    connect(m_deleteSelection,       SIGNAL(triggered()), this, SLOT(deleteSelection()));
    connect(m_hideImagesNotInRegion, SIGNAL(triggered()), this, SLOT(hideImagesNotInRegion()));
    connect(m_saveVectorLayer,       SIGNAL(triggered()), this, SLOT(saveVectorLayer()));
    connect(m_deleteVertex,          SIGNAL(triggered()), this, SLOT(deleteVertex()));
    connect(m_deleteVertices,        SIGNAL(triggered()), this, SLOT(deleteVertices()));
    connect(m_insertVertex,          SIGNAL(triggered()), this, SLOT(insertVertex()));
    connect(m_mergePolys,            SIGNAL(triggered()), this, SLOT(mergePolys()));

    MainWidget::maybeGenHillshade();
    
  } // End constructor


  MainWidget::~MainWidget() {
  }

  bool MainWidget::eventFilter(QObject *obj, QEvent *E){
    return QWidget::eventFilter(obj, E);
  }

  // What will happen when the user right-clicks on the table
  // listing the files.
  void MainWidget::customMenuRequested(QPoint pos){

    // Process user's choice from m_chooseFilesDlg.
    if (!m_chooseFilesDlg)
      return;

    QTableWidget * filesTable = m_chooseFilesDlg->getFilesTable();

    // Determine which row of the table the user clicked on
    QModelIndex tablePos=filesTable->indexAt(pos);
    int imageIndex = tablePos.row();

    // We will pass this index to the slots via this global variable
    m_indicesWithAction.clear();
    m_indicesWithAction.insert(imageIndex);
    
    QMenu *menu=new QMenu(this);

    m_toggleHillshadeFromTable = menu->addAction("Toggle hillshade display");
    connect(m_toggleHillshadeFromTable, SIGNAL(triggered()), this, SLOT(refreshHillshade()));
    
    m_bringImageOnTopFromTable = menu->addAction("Bring image on top");
    connect(m_bringImageOnTopFromTable, SIGNAL(triggered()), this, SLOT(bringImageOnTopSlot()));

    m_pushImageToBottomFromTable = menu->addAction("Push image to bottom");
    connect(m_pushImageToBottomFromTable, SIGNAL(triggered()), this, SLOT(pushImageToBottomSlot()));

    m_zoomToImageFromTable = menu->addAction("Zoom to image");
    connect(m_zoomToImageFromTable, SIGNAL(triggered()), this, SLOT(zoomToImage()));

    m_deleteImage = menu->addAction("Delete image");
    connect(m_deleteImage, SIGNAL(triggered()), this, SLOT(deleteImage()));

    // If having shapefiles, make it possible to change their colors
    bool has_shp = false;
    for (size_t it = 0; it < m_images.size(); it++) {
      if (vw::get_extension(m_images[it].name) == ".shp")
        has_shp = true;
    }
    if (has_shp) {
      m_changePolyColor = menu->addAction("Change colors of polygons");
      connect(m_changePolyColor, SIGNAL(triggered()), this, SLOT(changePolyColor()));
    }
    
    menu->exec(filesTable->mapToGlobal(pos));
  }

  void MainWidget::showFilesChosenByUser(int rowClicked, int columnClicked){

    // Process user's choice from m_chooseFilesDlg.
    if (!m_chooseFilesDlg)
      return;

    QTableWidget * filesTable = m_chooseFilesDlg->getFilesTable();
    int rows = filesTable->rowCount();

    // If we did not click on the checkbox, but on the image name,
    // make it checked
    if (columnClicked > 0) {
      QTableWidgetItem *item = filesTable->item(rowClicked, 0);
      item->setCheckState(Qt::Checked);
    }

    updateFilesToHide();

    // If we just checked a certain image, it will be shown on top of the other ones.
    QTableWidgetItem *item = filesTable->item(rowClicked, 0);
    if (item->checkState() == Qt::Checked){
      bringImageOnTop(rowClicked);
    }

    // If we clicked on the image name, zoom to it.
    if (columnClicked > 0) {
      // I could not use this functionality from a double click event.
      MainWidget::zoomToImageInTableCell(rowClicked, columnClicked);
    } else {
      refreshPixmap();
    }
    
    return;
  }

  // View next or previous image
  void MainWidget::viewOtherImage(int delta) {
    
    if (!m_chooseFilesDlg)
      return;

    if (delta != -1 && delta != 1) 
      return;

    QTableWidget * filesTable = m_chooseFilesDlg->getFilesTable();
    int rows = filesTable->rowCount();
    
    if (rows == 0) 
      return;
    
    // First see how many images have a checkbox now, so are being shown
    std::set<int> shown;
    for (int rowIter = 0; rowIter < rows; rowIter++) {
      QTableWidgetItem *item = filesTable->item(rowIter, 0);
      if (item->checkState() == Qt::Checked)
        shown.insert(rowIter);
    }
    
    // If no images are being shown or more than one, show the first
    int shownRow = 0;
    if (shown.size() == 1) {
      // Else show the next or previous image. Note how we add 'rows'
      // before we find the remainder, as delta could be negative.
      shownRow = *shown.begin();
      shownRow = (shownRow + delta + rows) % rows;
    }
    
    // Show the next/previous one, and hide the rest 
    for (int rowIter = 0; rowIter < rows; rowIter++){
      QTableWidgetItem *item = filesTable->item(rowIter, 0);
      if (rowIter == shownRow)
        item->setCheckState(Qt::Checked);
      else
        item->setCheckState(Qt::Unchecked);
    }

    updateFilesToHide();

    refreshPixmap();
    
    return;
  }

  void MainWidget::viewNextImage(){
    MainWidget::viewOtherImage(1);
  }

  void MainWidget::viewPrevImage(){
    MainWidget::viewOtherImage(-1);
  }
  
  void MainWidget::zoomToImageInTableCell(int rowClicked, int columnClicked){
    // We will pass this index to the desired slot via this global variable
    m_indicesWithAction.clear();
    m_indicesWithAction.insert(rowClicked);

    // Do the actual work for given value
    zoomToImage();
  }
  
  void MainWidget::toggleAllOnOff(){

    // Process user's choice from m_chooseFilesDlg.
    if (!m_chooseFilesDlg)
      return;

    QTableWidget * filesTable = m_chooseFilesDlg->getFilesTable();
    int rows = filesTable->rowCount();

    // See if all files are hidden
    bool allOff = true;
    for (int rowIter = 0; rowIter < rows; rowIter++){
      QTableWidgetItem *item = filesTable->item(rowIter, 0);
      if (item->checkState() == Qt::Checked){
        allOff = false;
      }
    }

    // If all files are hidden, we will show all. Else hide all.
    m_filesToHide.clear();
    for (int rowIter = 0; rowIter < rows; rowIter++){
      QTableWidgetItem *item = filesTable->item(rowIter, 0);
      std::string fileName = (filesTable->item(rowIter, 1)->data(0)).toString().toStdString();

      if (allOff)
        item->setCheckState(Qt::Checked);
      else
        item->setCheckState(Qt::Unchecked);
    }

    updateFilesToHide();

    if (!allOff) {
      // Now all files are hidden, per above. So reset the order, so that when
      // we show them, they are in the original order.
      int num_images = m_images.size();
      m_filesOrder.resize(num_images);
      for (int i = 0; i < num_images; i++)
        m_filesOrder[i] = i;
    }
    
    // Force the horizontal scrollbar in the table to go left, so one can see
    // the checkboxes.
    QScrollBar * hScrollBar = filesTable->horizontalScrollBar();
    hScrollBar->triggerAction(QScrollBar::SliderToMinimum);
    
    refreshPixmap();
  }

  void MainWidget::updateFilesToHide() {

    if (!m_chooseFilesDlg)
      return;

    QTableWidget * filesTable = m_chooseFilesDlg->getFilesTable();
    int rows = filesTable->rowCount();

    // Refresh the list of all the unchecked files
    m_filesToHide.clear();
    for (int rowIter = 0; rowIter < rows; rowIter++){
      QTableWidgetItem *item = filesTable->item(rowIter, 0);
      if (item->checkState() != Qt::Checked) {
        std::string fileName = (filesTable->item(rowIter, 1)->data(0)).toString().toStdString();
        m_filesToHide.insert(fileName);
      }
    }
    
  }
  
  BBox2 MainWidget::expand_box_to_keep_aspect_ratio(BBox2 const& box) {
    
    BBox2 in_box = box;
    if (in_box.empty()) in_box = BBox2(0, 0, 1, 1); // if it came to worst

    BBox2 out_box = in_box;
    double aspect = double(m_window_width) / m_window_height;
    if (in_box.width() / in_box.height() < aspect) {
      // Width needs to grow
      double new_width = in_box.height() * aspect;
      double delta = (new_width - in_box.width())/2.0;
      out_box.min().x() -= delta; out_box.max().x() += delta;
    }else if (in_box.width() / in_box.height() > aspect) {
      // Height needs to grow
      double new_height = in_box.width() / aspect;
      double delta = (new_height - in_box.height())/2.0;
      out_box.min().y() -= delta; out_box.max().y() += delta;
    }

    return out_box;
  }

  vw::BBox2 MainWidget::worldBox() const {
    return m_world_box;
  }
  
  void MainWidget::setWorldBox(vw::BBox2 const& world_box) {
    m_world_box = world_box;
  }
  
  // Zoom to show each image fully.
  void MainWidget::sizeToFit() {

    m_current_view = expand_box_to_keep_aspect_ratio(m_world_box);
    
    // If this is the first time we draw the image, so right when
    // we started, invoke update() which will invoke paintEvent().
    // That one will not only call refreshPixmap() but will
    // also mark that it did so. This is a bit confusing, but it is
    // necessary since otherwise Qt will first call this function,
    // invoking refreshPixmap(), then will call update() one more time
    // invoking needlessly refreshPixmap() again, which is expensive.
    if (m_firstPaintEvent){
      update();
    }else {
      refreshPixmap();
    }
  }

  void MainWidget::viewUnthreshImages(){
    m_thresh_view_mode = false;
    MainWidget::setHillshadeMode(false);
    refreshPixmap();
  }

  // The region that is currently viewable, in the first image pixel domain
  BBox2 MainWidget::firstImagePixelBox() const{
    
    if (m_images.size() == 0) {
      // Must never happen
      vw_out() << "Did not expect no images!";
      vw_throw(ArgumentErr() << "Did not expect no images.\n");
    }
    return MainWidget::world2image(m_current_view, 0);
  }

  // The current image box in world coordinates
  BBox2 MainWidget::firstImageWorldBox(BBox2 const& image_box) const{
    if (m_images.size() == 0) {
      // Must never happen
      vw_out() << "Did not expect no images!";
      vw_throw(ArgumentErr() << "Did not expect no images.\n");
    }
    return MainWidget::image2world(image_box, 0);
  }
  
  void MainWidget::viewThreshImages(bool refresh_pixmap){
    m_thresh_view_mode = true;
    MainWidget::setHillshadeMode(false);

    int num_non_poly_images = 0;
    int num_images = m_images.size();
    for (int image_iter = 0; image_iter < num_images; image_iter++) {
      if (!m_images[image_iter].isPoly())
        num_non_poly_images++;
    }

    if (num_non_poly_images > 1) {
      if (std::isnan(asp::stereo_settings().nodata_value))
        popUp("Must have just one image in each window to view thresholded images.");
      else
        popUp("Must have just one image in each window to use the nodata option.");
        
      m_thresh_view_mode = false;

      refreshPixmap();
      return;
    }

    m_thresh_images.clear(); // wipe the old copy
    m_thresh_images.resize(num_images);

    // Create the thresholded images and save them to disk. We have to do it each
    // time as perhaps the image threshold changed.
    for (int image_iter = 0; image_iter < num_images; image_iter++) {
      std::string input_file = m_images[image_iter].name;

      if (m_images[image_iter].isPoly())
        continue;
      
      double nodata_val = -std::numeric_limits<double>::max();
      vw::read_nodata_val(input_file, nodata_val);
      nodata_val = std::max(nodata_val, m_thresh);

      int num_channels = m_images[image_iter].img.planes();
      if (num_channels != 1) {
        popUp("Thresholding makes sense only for single-channel images.");
        m_thresh_view_mode = false;
        return;
      }

      ImageViewRef<double> thresh_image
        = apply_mask(create_mask_less_or_equal(DiskImageView<double>(input_file),
                                               nodata_val), nodata_val);

      std::string suffix = "_thresh.tif";
      bool has_georef = false;
      bool has_nodata = true;
      vw::cartography::GeoReference georef;
      std::string output_file
        = write_in_orig_or_curr_dir(m_opt,
                                    thresh_image, input_file, suffix,
                                    has_georef,  georef,
                                    has_nodata, nodata_val);

      // Read it back right away
      m_thresh_images[image_iter].read(output_file, m_opt);
      temporary_files().files.insert(output_file);
    }

    // We may not want to refresh the pixmap right away if we are going to
    // update the GUI anyway in proper time
    if (refresh_pixmap) 
      refreshPixmap();
  }

  void MainWidget::maybeGenHillshade(){

    int num_images = m_images.size();
    m_hillshaded_images.clear(); // wipe the old copy
    m_hillshaded_images.resize(num_images);

    // Create the hillshaded images and save them to disk. We have to do
    // it each time as perhaps the hillshade parameters changed.
    for (int image_iter = 0; image_iter < num_images; image_iter++) {

      if (!m_hillshade_mode[image_iter]) continue;

      if (!m_images[image_iter].has_georef) {
        popUp("Hill-shading requires georeferenced images.");
        m_hillshade_mode[image_iter] = false;
        return;
      }

      // Cannot hillshade a polygon
      if (m_images[image_iter].isPoly()) 
        continue;

      std::string input_file = m_images[image_iter].name;
      int num_channels = m_images[image_iter].img.planes();
      if (num_channels != 1) {
        // Turn off hillshade mode for all images which don't support it,
        // or else this error will keep on coming up
        for (int iter2 = 0; iter2 < num_images; iter2++) {
          int num_channels2 = m_images[iter2].img.planes();
          if (num_channels2 != 1) {
            m_hillshade_mode[iter2] = false;
          }
        }
        
        popUp("Hill-shading makes sense only for single-channel images.");
        return;
      }

      // Save the hillshaded images to disk
      std::string hillshaded_file;
      bool success = write_hillshade(m_opt,
                                     m_hillshade_azimuth,
                                     m_hillshade_elevation,
                                     input_file, hillshaded_file);
      if (!success) {
        m_hillshade_mode[image_iter] = false;
        return;
      }

      vw_out() << "Reading: " << hillshaded_file << std::endl;
      m_hillshaded_images[image_iter].read(hillshaded_file, m_opt);
      temporary_files().files.insert(hillshaded_file);
    }
  }

  // Delete an image from the list
  void MainWidget::deleteImage(){
    emit removeImageAndRefreshSignal();
  }

  // Change the color of given layer of polygons
  void MainWidget::changePolyColor(){
    std::string polyColor;
    bool ans = getStringFromGui(this,
                                "Polygonal line color",
                                "Polygonal line color",
                                polyColor, polyColor);
    if (!ans)
      return;
    
    if (polyColor == "") {
      popUp("The polygonal line color must be set.");
      return;
    }
    
    for (std::set<int>::iterator it = m_indicesWithAction.begin();
	 it != m_indicesWithAction.end(); it++) {

      // We will assume if the user wants to zoom to this image,
      // it should be on top.
      bringImageOnTop(*it);
      m_perImagePolyColor[*it] = polyColor;
    }

    // This is no longer needed
    m_indicesWithAction.clear();

    // Redraw everything, which will change the color
    refreshPixmap();
  }
  
  // Allow the user to select multiple windows. 
  void MainWidget::allowMultipleSelections(){
    m_allowMultipleSelections = !m_allowMultipleSelections;
    m_allowMultipleSelections_action->setChecked(m_allowMultipleSelections);
    if (!m_allowMultipleSelections) {
      m_selectionRectangles.clear();
      refreshPixmap();
    }
  }

  void MainWidget::refreshHillshade(){

    for (std::set<int>::iterator it = m_indicesWithAction.begin();
	 it != m_indicesWithAction.end(); it++) {
      m_hillshade_mode[*it] = !m_hillshade_mode[*it];

      // We will assume if the user wants to see the hillshade
      // status of this image change, he'll also want it on top.
      bringImageOnTop(*it); 
    }

    m_thresh_calc_mode = false;
    m_thresh_view_mode = false;
    MainWidget::maybeGenHillshade();

    m_indicesWithAction.clear();
    refreshPixmap();
  }

  void MainWidget::zoomToImage(){

    for (std::set<int>::iterator it = m_indicesWithAction.begin();
	 it != m_indicesWithAction.end(); it++) {

      // We will assume if the user wants to zoom to this image,
      // it should be on top.
      bringImageOnTop(*it); 

      // Set the view window to be the region encompassing the image
      m_current_view = expand_box_to_keep_aspect_ratio
        (MainWidget::image2world(m_images[*it].image_bbox, *it));
    }

    // This is no longer needed
    m_indicesWithAction.clear();

    // Redraw in the computed window
    refreshPixmap();
  }

  void MainWidget::bringImageOnTopSlot(){

    for (std::set<int>::iterator it = m_indicesWithAction.begin();
	 it != m_indicesWithAction.end(); it++) {
      bringImageOnTop(*it); 
    }
    
    m_indicesWithAction.clear();

    refreshPixmap();
  }

  void MainWidget::pushImageToBottomSlot(){

    for (std::set<int>::iterator it = m_indicesWithAction.begin();
	 it != m_indicesWithAction.end(); it++) {
      pushImageToBottom(*it); 
    }
    
    m_indicesWithAction.clear();

    refreshPixmap();
  }

  void MainWidget::viewHillshadedImages(bool hillshade_mode){
    MainWidget::setHillshadeMode(hillshade_mode);
    refreshHillshade();
  }

  void MainWidget::toggleHillshade(){
    m_hillshade_mode.resize(m_images.size());
    for (size_t image_iter = 0; image_iter < m_hillshade_mode.size(); image_iter++) 
      m_hillshade_mode[image_iter] = !m_hillshade_mode[image_iter];

    refreshHillshade();
  }

  bool MainWidget::hillshadeMode() const {
    if (m_hillshade_mode.empty()) return false;

    // If we have to return just one value, one image not being
    // hillshaded will imply that the value is false
    for (size_t it = 0; it < m_hillshade_mode.size(); it++) {
      if (!m_hillshade_mode[it]) return false;
    }
    return true;
  }

  // Each image can be hillshaded independently of the others
  void MainWidget::setHillshadeMode(bool hillshade_mode){
    m_hillshade_mode.resize(m_images.size());
    for (size_t image_iter = 0; image_iter < m_hillshade_mode.size(); image_iter++) 
      m_hillshade_mode[image_iter] = hillshade_mode;
  }

  // Ensure the current image is displayed. Note that this on its own
  // does not refresh the view as refreshPixmap() is not called.
  void MainWidget::showImage(std::string const& image_name){
    std::set<std::string>::iterator it2 = m_filesToHide.find(image_name);
    if (it2 != m_filesToHide.end()){
      m_filesToHide.erase(it2);

      // Then turn on the checkbox in the table
      QTableWidget * filesTable = m_chooseFilesDlg->getFilesTable();
      int rows = filesTable->rowCount();
      
      for (int rowIter = 0; rowIter < rows; rowIter++){
        QTableWidgetItem *item = filesTable->item(rowIter, 0);
        std::string image_name2 = (filesTable->item(rowIter, 1)->data(0)).toString().toStdString();
        if (image_name == image_name2) {
          item->setCheckState(Qt::Checked);
        }
      }
    }
  }
  
  // The image with the given index will be on top when shown.
  void MainWidget::bringImageOnTop(int image_index){
    std::vector<int>::iterator it = std::find(m_filesOrder.begin(), m_filesOrder.end(), image_index);
    if (it != m_filesOrder.end()){
      m_filesOrder.erase(it);
      m_filesOrder.push_back(image_index); // show last, so on top
    }

    // The image should be visible
    MainWidget::showImage(m_images[image_index].name);
  }

  // The image with the given index will be on top when shown.
  void MainWidget::pushImageToBottom(int image_index){
    std::vector<int>::iterator it = std::find(m_filesOrder.begin(), m_filesOrder.end(), image_index);
    if (it != m_filesOrder.end()){
      m_filesOrder.erase(it);
      m_filesOrder.insert(m_filesOrder.begin(), image_index); // show first, so at the bottom
    }

    // The image should be visible
    MainWidget::showImage(m_images[image_index].name);
  }
  
  // Convert the crop window to original pixel coordinates from
  // pixel coordinates on the screen.
  // TODO: Make screen2world() do it, to take an input a QRect (or BBox2)
  // and return as output the converted box.
  bool MainWidget::get_crop_win(QRect & win) {

    if (m_images.size() != 1) {
      popUp("Must have just one image in each window to be able to select regions for stereo.");
      m_cropWinMode = false;
      m_rubberBand = m_emptyRubberBand;
      m_stereoCropWin = BBox2();
      refreshPixmap();
      return false;
    }

    if (m_stereoCropWin.empty()) {
      popUp("No valid region for stereo is present. Regions can be selected with Control-Mouse in each image.");
      return false;
    }

    win = bbox2qrect(world2image(m_stereoCropWin, 0));
    return true;
  }

  void MainWidget::zoom(double scale) {

    updateCurrentMousePosition();
    scale = std::max(1e-8, scale);
    BBox2 current_view = (m_current_view - m_curr_world_pos) / scale
      + m_curr_world_pos;

    if (!current_view.empty()){
      // Check to make sure we haven't hit our zoom limits...
      m_current_view = current_view;
      m_can_emit_zoom_all_signal = true;
      refreshPixmap();
    }
  }

  void MainWidget::resizeEvent(QResizeEvent*){
    QRect v       = this->geometry();
    m_window_width  = std::max(v.width(), 1);
    m_window_height = std::max(v.height(), 1);
    sizeToFit();
    return;
  }

  // --------------------------------------------------------------
  //             MainWidget Private Methods
  // --------------------------------------------------------------

  // TODO: Rewrite this to draw only the pixels that end up being seen!
  // It should greatly improve the performance!
  // Also, the buffer to be rendered can be split into tiles,
  // with each tile being rendered in its own thread.
  void MainWidget::drawImage(QPainter* paint) {

    // Sometimes we arrive here prematurely, before the window geometry was
    // determined. Then, there is nothing to do.

    if (m_current_view.empty()) return;
    
    Stopwatch sw1;
    sw1.start();

    // Loop through input images
    // - These images get drawn in the same
    for (size_t j = 0; j < m_images.size(); j++){

      //Stopwatch sw2;
      //sw2.start();

      int i = m_filesOrder[j];

      // Don't show files the user wants hidden
      std::string fileName = m_images[i].name;
      if (m_filesToHide.find(fileName) != m_filesToHide.end())
        continue;

      // The portion of the image in the current view. 
      BBox2 curr_world_box = m_current_view;

      BBox2 B = MainWidget::image2world(m_images[i].image_bbox, i);

      curr_world_box.crop(B);

      // This is a bugfix for the case when the world boxes 
      // of images do not overlap.
      if (curr_world_box.empty())
        continue;

      // See where it fits on the screen
      BBox2i screen_box;
      screen_box.grow(floor(world2screen(curr_world_box.min())));
      screen_box.grow(ceil(world2screen(curr_world_box.max())));

      // Ensure the screen box is never empty
      if (screen_box.min().x() >= screen_box.max().x())
        screen_box.max().x() = screen_box.min().x() + 1;
      if (screen_box.min().y() >= screen_box.max().y())
        screen_box.max().y() = screen_box.min().y() + 1;

      // Go from world coordinates to pixels in the second image.
      BBox2 image_box = MainWidget::world2image(curr_world_box, i);

      // Grow a bit to integer, as otherwise we get strange results
      // if zooming too close.
      image_box.min() = floor(image_box.min());
      image_box.max() = ceil(image_box.max());

      if (m_images[i].isPoly())
        continue;

      QImage qimg;
      // Since the image portion contained in image_box could be huge,
      // but the screen area small, render a sub-sampled version of
      // the image for speed.
      // Convert to double before multiplication, to avoid overflow
      // when multiplying large integers.
      double scale = sqrt((1.0*image_box.width()) * image_box.height())/
        std::max(1.0, sqrt((1.0*screen_box.width()) * screen_box.height()));
      double scale_out;
      BBox2i region_out;
      bool   highlight_nodata = m_thresh_view_mode;
      if (!std::isnan(asp::stereo_settings().nodata_value)) {
        // When the user specifies --nodata-value, we will show
        // nodata pixels as transparent.
        highlight_nodata = false;
      }
      
      //sw2.stop();
      //vw_out() << "Render time 2 (seconds): " << sw2.elapsed_seconds() << std::endl;

      //Stopwatch sw3;
      //sw3.start();
      
      if (m_thresh_view_mode){
        m_thresh_images[i].img.get_image_clip(scale, image_box,
                                              highlight_nodata,
                                              qimg, scale_out, region_out);
      }else if (m_hillshade_mode[i]){
        m_hillshaded_images[i].img.get_image_clip(scale, image_box,
                                                  highlight_nodata,
                                                  qimg, scale_out, region_out);
      }else{
        // Original images
        m_images[i].img.get_image_clip(scale, image_box,
                                       highlight_nodata,
                                       qimg, scale_out, region_out);
      }

      //sw3.stop();
      //vw_out() << "Render time 3 (seconds): " << sw3.elapsed_seconds() << std::endl;

      // Draw on image screen
      if (!m_use_georef){
        //Stopwatch sw4;
        //sw4.start();
        // This is a regular image, no georeference, just pass it to the QT painter
        QRect rect(screen_box.min().x(), screen_box.min().y(),
                   screen_box.width(), screen_box.height());
        paint->drawImage(rect, qimg);
        //sw4.stop();
        //vw_out() << "Render time 4 (seconds): " << sw4.elapsed_seconds() << std::endl;
        
      }else{

        // Overlay georeferenced images
        
        //Stopwatch sw5;
        //sw5.start();
        
        // We fetched a bunch of pixels at some scale.
        // Need to place them on the screen at given projected position.
        // - To do that we will fill up this QImage object with interpolated data, then paint it.
        QImage qimg2 = QImage(screen_box.width(), screen_box.height(),
                              QImage::Format_ARGB32_Premultiplied);

        // Initialize all pixels to transparent
        for (int col = 0; col < qimg2.width(); col++) {
          for (int row = 0; row < qimg2.height(); row++) {
            qimg2.setPixel(col, row,  QColor(0, 0, 0, 0).rgba());
          }
        }

        //sw5.stop();
        //vw_out() << "Render time 5 (seconds): " << sw5.elapsed_seconds() << std::endl;
        
        //Stopwatch sw6;
        //sw6.start();
    
#pragma omp parallel for
        for (int x = screen_box.min().x(); x < screen_box.max().x(); x++){
          for (int y = screen_box.min().y(); y < screen_box.max().y(); y++){

            // Convert from a pixel as seen on screen to the world coordinate system.
            Vector2 world_pt = screen2world(Vector2(x, y));

            // p is in pixel coordinates of m_images[i]
            Vector2 p;
            try {
              p = MainWidget::world2image(world_pt, i);
              bool is_in = (p[0] >= 0 && p[0] <= m_images[i].img.cols()-1 &&
                            p[1] >= 0 && p[1] <= m_images[i].img.rows()-1 );
              if (!is_in) continue; // out of range
            }catch ( const std::exception & e ) {
              continue;
            }

            // Convert to scaled image pixels and snap to integer value
            // TODO(oalexan1): This may introduce subpixel artifacts.
            p = round(p/scale_out);

            if (!region_out.contains(p)) continue; // out of range again

            int px = p.x() - region_out.min().x();
            int py = p.y() - region_out.min().y();
            if (px < 0 || py < 0 || px >= qimg.width() || py >= qimg.height() ){
              vw_out() << "Book-keeping failure!";
              vw_throw(ArgumentErr() << "Book-keeping failure.\n");
            }
            qimg2.setPixel(x-screen_box.min().x(), // Fill the temp QImage object
                           y-screen_box.min().y(),
                           qimg.pixel(px, py));
          }
        } // End loop through pixels

        //sw6.stop();
        //vw_out() << "Render time 6 (seconds): " << sw6.elapsed_seconds() << std::endl;

        //Stopwatch sw7;
        //sw7.start();
        
        // Send the temp QImage object to the painter
        QRect rect(screen_box.min().x(), screen_box.min().y(),
                   screen_box.width(), screen_box.height());
        paint->drawImage(rect, qimg2);

        //sw7.stop();
        //vw_out() << "Render time 7 (seconds): " << sw7.elapsed_seconds() << std::endl;
        
      }

    } // End loop through input images

    sw1.stop();
    //vw_out() << "Render time (seconds): " << sw1.elapsed_seconds() << std::endl;
    
    return;
  } // End function drawImage()
  
  void MainWidget::drawInterestPoints(QPainter* paint){

    // Highlight colors for various actions
    QColor ipColor              = QColor(255,  0,  0); // Red
    QColor ipInvalidColor       = QColor(255,163, 26); // Orange
    QColor ipAddHighlightColor  = QColor( 64,255,  0); // Green
    QColor ipMoveHighlightColor = QColor(255,  0,255); // Magenta

    paint->setBrush(Qt::NoBrush);

    if ((m_images.size() != 1) && m_matchlist.getNumPoints() > 0) {
      // In order to be able to see matches, each image must be in its own widget.
      // So, if the current widget has more than an image, they are stacked on top
      // of each other, and then we just can't show IP.
      emit turnOffViewMatchesSignal();
      return;
    }

    // If this point is currently being edited by the user, highlight it.
    // - Here we check to see if it has not been placed in all images yet.
    size_t lastImage = m_matchlist.getNumImages()-1;
    bool highlight_last =
      (m_matchlist.getNumPoints(m_image_id) > m_matchlist.getNumPoints(lastImage));

    // Needed for image2word
    size_t trans_image_id = getTransformImageIndex();

    // For each IP...
    for (size_t ip_iter = 0; ip_iter < m_matchlist.getNumPoints(m_image_id); ip_iter++) {
      // Generate the pixel coord of the point
      Vector2 pt    = m_matchlist.getPointCoord(m_image_id, ip_iter);
      Vector2 world = MainWidget::image2world(pt, static_cast<int>(trans_image_id));
      Vector2 P     = world2screen(world);

      // Do not draw points that are outside the viewing area
      if (P.x() < 0 || P.x() > m_window_width ||
          P.y() < 0 || P.y() > m_window_height) {
        continue;
      }
      
      paint->setPen(ipColor); // The default IP color

      if (!m_matchlist.isPointValid(m_image_id, ip_iter))
        paint->setPen(ipInvalidColor);

      // Highlighting the last point
      if (highlight_last && (ip_iter == m_matchlist.getNumPoints(m_image_id)-1)) 
        paint->setPen(ipAddHighlightColor);

      if (static_cast<int>(ip_iter) == m_editMatchPointVecIndex)
        paint->setPen(ipMoveHighlightColor);

      QPoint Q(P.x(), P.y());
      paint->drawEllipse(Q, 2, 2); // Draw the point!

    } // End loop through points
  } // End function drawInterestPoints


  void MainWidget::updateCurrentMousePosition() {
    m_curr_world_pos = screen2world(m_curr_pixel_pos);
  }

  void MainWidget::setZoomAllToSameRegion(bool zoom_all_to_same_region){
    m_zoom_all_to_same_region = zoom_all_to_same_region;
  }

  vw::BBox2 MainWidget::current_view(){
    return m_current_view;
  }

  void MainWidget::zoomToRegion(vw::BBox2 const& region){
    if (region.empty()) {
      popUp("Cannot zoom to empty region.");
      return;
    }
    m_current_view = expand_box_to_keep_aspect_ratio(region);

    refreshPixmap();
  }

  // --------------------------------------------------------------
  //             MainWidget Event Handlers
  // --------------------------------------------------------------

  void MainWidget::refreshPixmap(){

    // This is an expensive function. It will completely redraw
    // what is on the screen. For that reason, don't draw directly on
    // the screen, but rather into m_pixmap, which we use as a cache.

    // If just tiny redrawings are necessary, such as updating the
    // rubberband, simply pull the view from this cache,
    // and update the rubberband on top of it. This technique
    // is a well-known design pattern in Qt.

    if (m_zoom_all_to_same_region && m_can_emit_zoom_all_signal){
      m_can_emit_zoom_all_signal = false;
      emit zoomAllToSameRegionSignal(m_image_id);

      // Now we call the parent, which will set the zoom window,
      // and call back here for all widgets.
      return;
    }

    m_pixmap = QPixmap(size());
    m_pixmap.fill(m_backgroundColor);

    QPainter paint(&m_pixmap);
    paint.initFrom(this);

    //QFont F;
    //F.setPointSize(m_prefs.fontSize);
    //F.setStyleStrategy(QFont::NoAntialias);
    //paint.setFont(F);
    MainWidget::drawImage(&paint);

    // Invokes MainWidget::PaintEvent().
    update();

    return;
  }

  void MainWidget::paintEvent(QPaintEvent * /* event */) {

    if (m_firstPaintEvent){
      // This will be called the very first time the display is
      // initialized. We will paint into the pixmap, and
      // then display the pixmap on the screen.
      m_firstPaintEvent = false;
      refreshPixmap();
    }

    // Note that we draw from the cached pixmap, instead of redrawing
    // the image from scratch.
    QPainter paint(this);
    paint.drawPixmap(0, 0, m_pixmap);

    QColor      rubberBandColor = QColor("yellow");
    QColor      cropWinColor    = QColor("red");
    std::string polyColorStr    = m_polyColor;
    QColor      polyColor       = QColor(polyColorStr.c_str());

    // We will color the rubberband in the crop win color if we are
    // in crop win mode.
    if (m_cropWinMode)
      paint.setPen(cropWinColor);
    else
      paint.setPen(rubberBandColor);

    // Draw the rubberband. We adjust by subtracting 1 from right and
    // bottom corner below to be consistent with updateRubberBand(), as
    // rect.bottom() is rect.top() + rect.height()-1.
    paint.drawRect(m_rubberBand.normalized().adjusted(0, 0, -1, -1));

    // Draw the stereo crop window.  Note that the stereo crop window
    // may exist independently of whether the rubber band exists.
    if (!m_stereoCropWin.empty()) {
      QRect R = bbox2qrect(world2screen(m_stereoCropWin));
      paint.setPen(cropWinColor);
      paint.drawRect(R.normalized().adjusted(0, 0, -1, -1));
    }

    // If we allow multiple selection windows
    for (size_t win = 0; win < m_selectionRectangles.size(); win++) {
      QRect R = bbox2qrect(world2screen(m_selectionRectangles[win]));
      paint.setPen(cropWinColor);
      paint.drawRect(R.normalized().adjusted(0, 0, -1, -1));
    }

    // TODO(oalexan1): All the logic below must be in its own function,
    // called for example plotPolygons().
    // Also replace plotDPoly() with plotPoly().
    // When deleting vertices need to use a georef as well.
    
    bool plotPoints    = false, plotEdges = true, plotFilled = false;
    int  drawVertIndex = 0, lineWidth = m_lineWidth;
    bool isPolyClosed  = false;
    std::string layer  = "";
    
    // Plot the polygonal line which we are profiling
    if (m_profileMode) {
      vw::geometry::dPoly poly;
      poly.appendPolygon(m_profileX.size(),  
                         vw::geometry::vecPtr(m_profileX),  
                         vw::geometry::vecPtr(m_profileY),  
                         isPolyClosed, polyColorStr, layer);
      bool showIndices = false;
      MainWidget::plotDPoly(plotPoints, plotEdges, plotFilled, showIndices,
                            lineWidth,  
                            drawVertIndex, polyColor, paint,  
                            poly);
    }

    // TODO(oalexan1): Should the persistent polygons be drawn
    // as part of the drawImage() call? How about polygons
    // actively being edited?
    
    // Loop through the input images. Plot the polygons. Note how we
    // add one more fake image at the end to take care of the polygon
    // we are in the middle of drawing.
    for (size_t j = 0; j < m_images.size() + 1; j++){

      bool currDrawnPoly = (j == m_images.size());
      
      int image_it = j;
      if (!currDrawnPoly) {
        image_it = m_filesOrder[j];
      
        // Don't show files the user wants hidden
        std::string fileName = m_images[image_it].name;
        if (m_filesToHide.find(fileName) != m_filesToHide.end())
          continue;
      }

      // Let polyVec be the polygons for the current image, or,
      // at the end, the polygon we are in the middle of drawing
      // TODO(oalexan1): How to avoid a deep copy?
      std::vector<vw::geometry::dPoly> polyVec;
      if (!currDrawnPoly) {
        polyVec = m_images[image_it].polyVec; // deep copy
      } else {
          if (m_currPolyX.empty() || !m_polyEditMode)
            continue;
          
          if (!m_images[m_polyLayerIndex].has_georef) // this should not happen
            vw_throw(ArgumentErr() << "Expecting images with georeference.\n");
          
          vw::geometry::dPoly poly;
          poly.reset();
          poly.appendPolygon(m_currPolyX.size(),  
                             vw::geometry::vecPtr(m_currPolyX),  
                             vw::geometry::vecPtr(m_currPolyY),  
                             isPolyClosed, polyColorStr, layer);
          polyVec.push_back(poly);
      }

      
      // See if to use a custom color for this polygon
      QColor currPolyColor = polyColor;
      auto color_it = m_perImagePolyColor.find(image_it);
      if (color_it != m_perImagePolyColor.end()) {
        currPolyColor = QColor((color_it->second).c_str());
      }
      
      // Plot the polygon being drawn now, and pre-existing polygons
      for (size_t polyIter = 0; polyIter < polyVec.size(); polyIter++){
      
        vw::geometry::dPoly poly = polyVec[polyIter]; // make a deep copy
      
        double val1 = vw::geometry::signedPolyArea(poly.get_totalNumVerts(),
                                                   poly.get_xv(), poly.get_yv());

        // Convert to world units
        int            numVerts  = poly.get_totalNumVerts();
        double *             xv  = poly.get_xv();
        double *             yv  = poly.get_yv();
        for (int vIter = 0; vIter < numVerts; vIter++){

          Vector2 P;
          if (!currDrawnPoly) 
            P = projpoint2world(Vector2(xv[vIter], yv[vIter]), image_it);
          else
            P = projpoint2world(Vector2(xv[vIter], yv[vIter]), m_polyLayerIndex);

          xv[vIter] = P.x();
          yv[vIter] = P.y();
        }

        if (m_polyEditMode && m_moveVertex->isChecked()) {
          drawVertIndex = 1; // to draw a little square at each movable vertex
          plotPoints = true;
        }else{
          drawVertIndex = 0;
          plotPoints = false;
        }

        double val2 = vw::geometry::signedPolyArea(poly.get_totalNumVerts(),
                                                   poly.get_xv(), poly.get_yv());

        // If the conversion to world coords flips the orientation, correct for that.
        // TODO: This seems necessary. More thought is needed. 
        if (val1 * val2 < 0)
          poly.reverse();

        MainWidget::plotDPoly(plotPoints, plotEdges, m_showPolysFilled->isChecked(),
                              m_showIndices->isChecked(),
                              lineWidth, drawVertIndex, currPolyColor, paint, poly);
      }
    } // end iterating over polygons for all images
    
    // Call another function to handle drawing the interest points
    if ((static_cast<size_t>(m_image_id) < m_matchlist.getNumImages()) && m_view_matches) 
      drawInterestPoints(&paint);
    
  } // end paint event
  
  // Call paintEvent() on the edges of the rubberband
  void MainWidget::updateRubberBand(QRect & R){
    QRect rect = R.normalized();
    if (rect.width() > 0 || rect.height() > 0) {
      update(rect.left(),  rect.top(),    rect.width(), 1             );
      update(rect.left(),  rect.top(),    1,            rect.height() );
      update(rect.left(),  rect.bottom(), rect.width(), 1             );
      update(rect.right(), rect.top(),    1,            rect.height() );
    }
    return;
  }


  // We assume the user picked n points in the image.
  // Draw n-1 segments in between them. Plot the obtained profile.
  void MainWidget::plotProfile(std::vector<imageData> const& images,
                               // indices in the image to profile
                               std::vector<double> const& profileX, 
                               std::vector<double> const& profileY){

    if (images.empty()) return; // nothing to do

    // Create the profile window
    if (m_profilePlot == NULL) 
      m_profilePlot = new ProfilePlotter(this);

    int imgInd = 0; // just one image is present
    double nodata_val = images[imgInd].img.get_nodata_val();
    
    m_valsX.clear(); m_valsY.clear();
    int count = 0;
    
    int num_pts = profileX.size();
    for (int pt_iter = 0; pt_iter < num_pts; pt_iter++) {

      // Nothing to do if we are at the last point, unless
      // there is only one point.
      if (num_pts > 1 && pt_iter == num_pts - 1) continue;
	
      Vector2 begP = MainWidget::world2image(Vector2(profileX[pt_iter], profileY[pt_iter]),
                                             imgInd);

      Vector2 endP;
      if (num_pts == 1) {
        endP = begP; // only one point is present
      }else{
        endP = MainWidget::world2image(Vector2(profileX[pt_iter+1], profileY[pt_iter+1]), imgInd);
      }
      
      int begX = begP.x(),   begY = begP.y();
      int endX = endP.x(),   endY = endP.y();
      int seg_len = std::abs(begX - endX) + std::abs(begY - endY);
      if (seg_len == 0) seg_len = 1; // ensure it is never empty
      for (int p = 0; p <= seg_len; p++) {
        double t = double(p)/seg_len;
        int x = round( begX + t*(endX - begX) );
        int y = round( begY + t*(endY - begY) );
        bool is_in = (x >= 0 && x <= images[imgInd].img.cols()-1 &&
                      y >= 0 && y <= images[imgInd].img.rows()-1 );
        if (!is_in)
          continue;

        double pixel_val = images[imgInd].img.get_value_as_double(x, y);

        // TODO: Deal with this NAN
        if (pixel_val == nodata_val)
          pixel_val = std::numeric_limits<double>::quiet_NaN();
        m_valsX.push_back(count);
        m_valsY.push_back(pixel_val);
        count++;
      }

    }

    if (num_pts == 1) {
      // Just one point, really
      m_valsX.resize(1);
      m_valsY.resize(1);
    }
    
    // Wipe whatever was there before
    m_profilePlot->detachItems(); 
    
    QwtPlotCurve * curve = new QwtPlotCurve("1D Profile");
    m_profilePlot->setFixedWidth(300);
    m_profilePlot->setWindowTitle("1D Profile");

    if (!m_valsX.empty()) {
      
      double min_x = *std::min_element(m_valsX.begin(), m_valsX.end());
      double max_x = *std::max_element(m_valsX.begin(), m_valsX.end());
      double min_y = *std::min_element(m_valsY.begin(), m_valsY.end());
      double max_y = *std::max_element(m_valsY.begin(), m_valsY.end());

      // Ensure the window is always valid
      double small = 0.1;
      if (min_x == max_x) {
        min_x -= small; max_x += small;
      }
      if (min_y == max_y) {
        min_y -= small; max_y += small;
      }

      // Plot a point as a fat dot
      if (num_pts == 1)  {
        curve->setStyle(QwtPlotCurve::Dots);
      }
      
      curve->setData(new QwtCPointerData(&m_valsX[0], &m_valsY[0], m_valsX.size()));
      curve->setPen(* new QPen(Qt::red));
      curve->attach(m_profilePlot);
      
      double delta = 0.1;  // expand a bit right to see more x and y labels
      double widx = max_x - min_x;
      double widy = max_y - min_y;
      m_profilePlot->setAxisScale(QwtPlot::xBottom, min_x - delta*widx, max_x + delta*widx);
      m_profilePlot->setAxisScale(QwtPlot::yLeft,   min_y - delta*widy, max_y + delta*widy);
    }

    // Finally, refresh the plot
    m_profilePlot->replot();
    m_profilePlot->show();
  }
  
  void MainWidget::setProfileMode(bool profile_mode){
    m_profileMode = profile_mode;

    if (!m_profileMode) {
      // Clean up any profiling related info
      m_profileX.clear();
      m_profileY.clear();

      // Close the window. 
      if (m_profilePlot != NULL) {
        m_profilePlot->close();
        m_profilePlot->deleteLater();
        delete m_profilePlot;
        m_profilePlot = NULL;
      }

      // Call back to the main window and tell it to uncheck the profile
      // mode checkbox.
      emit uncheckProfileModeCheckbox();
      return;
    }else{

      bool refresh = true;
      setPolyEditMode(false, refresh);
      
      // Show the profile window
      MainWidget::plotProfile(m_images, m_profileX, m_profileY);
    }

    refreshPixmap();
  }

  void MainWidget::setPolyEditMode(bool polyEditMode, bool refresh){
    m_polyEditMode = polyEditMode;

    // Turn off moving vertices any time we turn on or off poly editing
    m_moveVertex->setChecked(false);
    m_showIndices->setChecked(false);

    if (!m_polyEditMode) {
      // Clean up any unfinished polygon
      // Need to put here pop-up asking to save
      m_currPolyX.clear();
      m_currPolyY.clear();

      // Call back to the main window and tell it to uncheck the polyEditMode checkbox
      // mode checkbox.
      emit uncheckPolyEditModeCheckbox();
      return;
    }else{
      setProfileMode(false);
    }

    // On occasions we don't want to force a refresh prematurely, the
    // GUI will take care of it when the layout is created
    if (refresh) 
      refreshPixmap();
  }

  // Convert a length in pixels to a length in world coordinates
  double MainWidget::pixelToWorldDist(double pd){
    
    Vector2 p = screen2world(Vector2(0, 0));
    Vector2 q = screen2world(Vector2(pd, 0));
    
    return norm_2(p-q);
  }
  
  void MainWidget::appendToPolyVec(vw::geometry::dPoly const& P){
    
    // Append the new polygon to the list of polygons. If we have several
    // clips already, append it to the last clip. If we have no clips,
    // create a new clip.
    if (m_images[m_polyLayerIndex].polyVec.size() == 0){
      m_images[m_polyLayerIndex].polyVec.push_back(P);
    }else{
      m_images[m_polyLayerIndex].polyVec.back().appendPolygons(P);
    }
    
    return;
  }
  
  // Add a point to the polygon being drawn or stop drawing and append
  // the drawn polygon to the list of polygons. This polygon is in the
  // world coordinate system. When we append it, we will convert it to
  // points in the desired geodetic projection.
  void MainWidget::addPolyVert(double px, double py){

    Vector2 S(px, py); // current point in screen pixels
    int pSize = m_currPolyX.size();

    // Starting point in this polygon. It is absolutely essential that we
    // keep it in world units. Otherwise, if we zoom while the polygon
    // is being drawn, we will not be able to close it properly.
    if (pSize == 0)
      m_startPix = screen2world(S); 
    
    if (pSize <= 0 || norm_2(world2screen(m_startPix) - S) > m_pixelTol ){
      
      // We did not arrive yet at the starting point of the polygon being
      // drawn. Add the current point.

      if (!m_images[m_polyLayerIndex].has_georef) // this should not happen
        vw_throw(ArgumentErr() << "Expecting images with georeference.\n"); 
      
      S = screen2world(S);                    // world coordinates
      m_world_box.grow(S); // to not cut when plotting later
      S = world2projpoint(S, m_polyLayerIndex); // projected units
      
      m_currPolyX.push_back(S.x());
      m_currPolyY.push_back(S.y());
      pSize = m_currPolyX.size();
      
      // This will call paintEvent which will draw the current poly line
      update();

      return;
    }

    // Form the newly finished polygon
    vw::geometry::dPoly poly;
    poly.reset();
    bool isPolyClosed = true;
    std::string color, layer;
    poly.appendPolygon(pSize,
		    vw::geometry::vecPtr(m_currPolyX), vw::geometry::vecPtr(m_currPolyY),
                    isPolyClosed, color, layer);

    double val1 = vw::geometry::signedPolyArea(poly.get_totalNumVerts(),
					      poly.get_xv(), poly.get_yv());


    // If conversion to world units flips the orientation, that means
    // reverse the original polygon.
    // TODO: This looks like a hack. But it works. 
    vw::geometry::dPoly poly2 = poly;

    double val2 = vw::geometry::signedPolyArea(poly2.get_totalNumVerts(),
					      poly2.get_xv(), poly2.get_yv());

    // Convert to world units
    int            numVerts  = poly2.get_totalNumVerts();
    double *             xv  = poly2.get_xv();
    double *             yv  = poly2.get_yv();
    for (int vIter = 0; vIter < numVerts; vIter++){
      Vector2 P = projpoint2world(Vector2(xv[vIter], yv[vIter]), m_polyLayerIndex); 
      xv[vIter] = P.x();
      yv[vIter] = P.y();
    }
    
    val2 = vw::geometry::signedPolyArea(poly2.get_totalNumVerts(),
					poly2.get_xv(), poly2.get_yv());
    if (val1*val2 < 0)
      poly.reverse();
    
    appendToPolyVec(poly);
    
    m_currPolyX.clear();
    m_currPolyY.clear();

    update(); // redraw the just polygons, not the underlying images
    //refreshPixmap();
    
    return;
  }

  // Delete a vertex closest to where the user clicked.
  // TODO(oalexan1): This will fail when different polygons have
  // different georeferences.
  void MainWidget::deleteVertex(){

    Vector2 P = screen2world(Vector2(m_mousePrsX, m_mousePrsY));
    
    double min_x, min_y, min_dist;
    int clipIndex, polyVecIndex, polyIndexInCurrPoly, vertIndexInCurrPoly;
    MainWidget::findClosestPolyVertex(// inputs
                                      P.x(), P.y(), m_images,
                                      // outputs
                                      clipIndex,
                                      polyVecIndex,
                                      polyIndexInCurrPoly,
                                      vertIndexInCurrPoly,
                                      min_x, min_y, min_dist);

    if (clipIndex           < 0 ||
        polyVecIndex        < 0 ||
        polyIndexInCurrPoly < 0 ||
        vertIndexInCurrPoly < 0)
      return;

    m_images[clipIndex].polyVec[polyVecIndex].eraseVertex
      (polyIndexInCurrPoly, vertIndexInCurrPoly);

    // This will redraw just the polygons, not the pixmap
    update();
    
    return;
  }

  void MainWidget::deleteVertices(){

    if (m_stereoCropWin.empty()) {
      popUp("No region is selected.");
      return;
    }

    // This code cannot be moved out to utilities since it calls the
    // function projpoint2world().
    
    for (size_t clipIter = 0; clipIter < m_images.size(); clipIter++) {
    
      for (size_t layerIter = 0; layerIter < m_images[clipIter].polyVec.size(); layerIter++) {
      
        vw::geometry::dPoly & poly     = m_images[clipIter].polyVec[layerIter]; // alias
        int                   numPolys = poly.get_numPolys();
        const int           * numVerts = poly.get_numVerts();
        const double        * xv       = poly.get_xv();
        const double        * yv       = poly.get_yv();
        std::vector<std::string>   colors   = poly.get_colors();
        std::vector<std::string>   layers   = poly.get_layers();

        vw::geometry::dPoly poly_out;
        int start = 0;
        for (int polyIter = 0; polyIter < numPolys; polyIter++){
        
          if (polyIter > 0) start += numVerts[polyIter - 1];
          int pSize = numVerts[polyIter];

          std::vector<double> out_xv, out_yv;
          for (int vIter = 0; vIter < pSize; vIter++){
            double  x = xv[start + vIter];
            double  y = yv[start + vIter];
            Vector2 P = projpoint2world(Vector2(x, y), clipIter);

            // This vertex will be deleted
            if (m_stereoCropWin.contains(P))
              continue;
            
            out_xv.push_back(x);
            out_yv.push_back(y);
          }

          // If there are no vertices left, or the polygon was not
          // degenerate before but becomes degenerate now, skip it.
          // (Polygons which were 1-point before are allowed.)
          if (out_xv.empty() || (pSize >= 3 && out_xv.size() < 3))
            continue; 

          bool isPolyClosed = true;
          poly_out.appendPolygon(out_xv.size(),  
                                 vw::geometry::vecPtr(out_xv),  
                                 vw::geometry::vecPtr(out_yv),  
                                 isPolyClosed, colors[polyIter], layers[polyIter]);
        }

        // Overwrite the polygon
        m_images[clipIter].polyVec[layerIter] = poly_out;
      }
    }
  
    // The selection has done its job, wipe it now
    m_stereoCropWin = BBox2();
    
    // This will redraw just the polygons, not the pixmap
    update();
  }
    
  // Find the closest edge in a given set of imageData structures to a
  // given point. This needs to be in the MainWidget class as it needs
  // to know about how to convert from world coordinates to each
  // imageData coordinates.
  void MainWidget::findClosestPolyEdge(// inputs
                                       double world_x0, double world_y0,
                                       std::vector<imageData> const& imageData,
                                       // outputs
                                       int & clipIndex,
                                       int & polyVecIndex,
                                       int & polyIndexInCurrPoly,
                                       int & vertIndexInCurrPoly,
                                       double & minX, double & minY,
                                       double & minDist){

    clipIndex           = -1;
    polyVecIndex        = -1;
    polyIndexInCurrPoly = -1;
    vertIndexInCurrPoly = -1;
    minX                = world_x0;
    minY                = world_y0;
    minDist             = std::numeric_limits<double>::max();

    Vector2 world_P(world_x0, world_y0);

    for (size_t clipIter = 0; clipIter < imageData.size(); clipIter++){
    
      double minX0, minY0, minDist0;
      int polyVecIndex0, polyIndexInCurrPoly0, vertIndexInCurrPoly0;
    
      // Convert from world coordinates to given clip coordinates
      Vector2 clip_P = world2projpoint(world_P, clipIter);
      
      vw::gui::findClosestPolyEdge(// inputs
                                   clip_P.x(), clip_P.y(),  
                                   imageData[clipIter].polyVec,  
                                   // outputs
                                   polyVecIndex0,  
                                   polyIndexInCurrPoly0,  
                                   vertIndexInCurrPoly0,  
                                   minX0, minY0,  
                                   minDist0);
      
      // Unless the polygon is empty, convert back to world
      // coordinates, and see if the current distance is smaller than
      // the previous one.
      if (polyVecIndex0 >= 0 && polyIndexInCurrPoly0 >= 0 &&
          vertIndexInCurrPoly0 >= 0) {

        Vector2 closest_P = projpoint2world(Vector2(minX0, minY0),
                                            clipIter);
        minDist0 = norm_2(closest_P - world_P);
        
        if (minDist0 <= minDist) {
          clipIndex           = clipIter;
          polyVecIndex        = polyVecIndex0;
          polyIndexInCurrPoly = polyIndexInCurrPoly0;
          vertIndexInCurrPoly = vertIndexInCurrPoly0;
          minDist             = minDist0;
          minX                = closest_P.x();
          minY                = closest_P.y();
        }
      }
    }

    return;
  }

  // Find the closest point in a given set of imageData structures to a given point
  // in world coordinates.
  void MainWidget::findClosestPolyVertex(// inputs
                                         double world_x0, double world_y0,
                                         std::vector<imageData> const& imageData,
                                         // outputs
                                         int & clipIndex,
                                         int & polyVecIndex,
                                         int & polyIndexInCurrPoly,
                                         int & vertIndexInCurrPoly,
                                         double & minX, double & minY,
                                         double & minDist){
    clipIndex           = -1;
    polyVecIndex        = -1;
    polyIndexInCurrPoly = -1;
    vertIndexInCurrPoly = -1;
    minX                = world_x0;
    minY                = world_y0;
    minDist             = std::numeric_limits<double>::max();

    Vector2 world_P(world_x0, world_y0);
    
    for (size_t clipIter = 0; clipIter < imageData.size(); clipIter++){
    
      double minX0, minY0, minDist0;
      int polyVecIndex0, polyIndexInCurrPoly0, vertIndexInCurrPoly0;

      // Convert from world coordinates to given clip coordinates
      Vector2 clip_P = world2projpoint(world_P, clipIter);

      vw::gui::findClosestPolyVertex(// inputs
                                     clip_P.x(), clip_P.y(),  
                                     imageData[clipIter].polyVec,  
                                     // outputs
                                     polyVecIndex0,  
                                     polyIndexInCurrPoly0,  
                                     vertIndexInCurrPoly0,  
                                     minX0, minY0,  
                                     minDist0);

      // Unless the polygon is empty, convert back to world
      // coordinates, and see if the current distance is smaller than
      // the previous one.
      if (polyVecIndex0 >= 0 && polyIndexInCurrPoly0 >= 0 &&
          vertIndexInCurrPoly0 >= 0) {
        
        Vector2 closest_P = projpoint2world(Vector2(minX0, minY0),
                                            clipIter);
        minDist0 = norm_2(closest_P - world_P);
      
        if (minDist0 <= minDist) {
          clipIndex           = clipIter;
          polyVecIndex        = polyVecIndex0;
          polyIndexInCurrPoly = polyIndexInCurrPoly0;
          vertIndexInCurrPoly = vertIndexInCurrPoly0;
          minDist             = minDist0;
          minX                = closest_P.x();
          minY                = closest_P.y();
        }
      }
    }
  
    return;
  }
  
  // Insert intermediate vertex where the mouse right-clicks.
  // TODO(oalexan1): This will fail when different polygons have
  // different georeferences.
  void MainWidget::insertVertex(){

    Vector2 P = screen2world(Vector2(m_mousePrsX, m_mousePrsY));

    m_world_box.grow(P); // to not cut when plotting later
    
    // If there is absolutely no polygon, start by creating one
    // with just one point.
    bool allEmpty = true;
    for (size_t clipIter = 0; clipIter < m_images.size(); clipIter++) {
      if (m_images[clipIter].polyVec.size() > 0 && 
          m_images[clipIter].polyVec[0].get_totalNumVerts() > 0) {
        allEmpty = false;
        break;
      }
    }
    
    if (allEmpty) {
      addPolyVert(m_mousePrsX, m_mousePrsY); // init the polygon
      addPolyVert(m_mousePrsX, m_mousePrsY); // declare the polygon finished
      return;
    }

    // The location of the point to be inserted looks more reasonable
    // when one searches for closest edge, not vertex. 
    double min_x, min_y, min_dist;
    int clipIndex, polyVecIndex, polyIndexInCurrPoly, vertIndexInCurrPoly;
    MainWidget::findClosestPolyEdge(// inputs
                                    P.x(), P.y(), m_images,
                                    // outputs
                                    clipIndex,
                                    polyVecIndex,
                                    polyIndexInCurrPoly,
                                    vertIndexInCurrPoly,
                                    min_x, min_y, min_dist);

    if (clipIndex           < 0 ||
        polyVecIndex        < 0 ||
        polyIndexInCurrPoly < 0 ||
        vertIndexInCurrPoly < 0) return;

    // Convert to coordinates of the desired clip
    P = world2projpoint(P, clipIndex);

    // Need +1 below as we insert AFTER current vertex
    m_images[clipIndex].polyVec[polyVecIndex].insertVertex(polyIndexInCurrPoly,
                                                           vertIndexInCurrPoly + 1,
                                                           P.x(), P.y());
    
    // This will redraw just the polygons, not the pixmap
    update();
    
    return;
  }

  // Merge some polygons and save them in imageData[outIndex]
  void MainWidget::mergePolys(std::vector<imageData> & imageData, int outIndex){

    std::vector<vw::geometry::dPoly> polyVec;

    try {
      // We will infer these from existing polygons
      std::string poly_color, layer_str;
    
      // We must first organize all those user-drawn curves into meaningful polygons.
      // This can flip orientations and order of polygons. 
      std::vector<OGRGeometry*> ogr_polys;

      for (size_t clipIter = 0; clipIter < imageData.size(); clipIter++) {

        auto & polyVec = imageData[clipIter].polyVec;
        for (size_t vecIter = 0; vecIter < polyVec.size(); vecIter++) {

          if (poly_color == ""){
            std::vector<std::string> colors = polyVec[vecIter].get_colors();
            if (!colors.empty()) 
              poly_color = colors[0];
          }
      
          if (layer_str == "") {
            std::vector<std::string> layers = polyVec[vecIter].get_layers();
            if (!layers.empty()) 
              layer_str = layers[0];
          }

          // Make a copy of the polygons
          vw::geometry::dPoly poly = polyVec[vecIter]; 
    
          double * xv         = poly.get_xv();
          double * yv         = poly.get_yv();
          const int* numVerts = poly.get_numVerts();
          int numPolys        = poly.get_numPolys();
          int  totalNumVerts  = poly.get_totalNumVerts();

          // Convert from the coordinate system of each layer
          // to the one of the output layer
          for (int vIter = 0; vIter < totalNumVerts; vIter++) {
            Vector2 P = projpoint2world(Vector2(xv[vIter], yv[vIter]), clipIter);
            P = world2projpoint(P, outIndex);
            xv[vIter] = P.x();
            yv[vIter] = P.y();
          }
          
          // Iterate over polygon rings in the given polygon set
          int startPos = 0;
          for (int pIter = 0; pIter < numPolys; pIter++){
      
            if (pIter > 0) startPos += numVerts[pIter - 1];
            int numCurrPolyVerts = numVerts[pIter];
      
            OGRLinearRing R;
            vw::geometry::toOGR(xv, yv, startPos, numCurrPolyVerts, R);

            OGRPolygon * P = new OGRPolygon;
            if (P->addRing(&R) != OGRERR_NONE )
              vw_throw(ArgumentErr() << "Failed add ring to polygon.\n");

            ogr_polys.push_back(P);
          }
    
        }
      }
    
      // The doc of this function says that the elements in ogr_polys will
      // be taken care of. We are responsible only for the vector of pointers
      // and for the output of this function.
      int pbIsValidGeometry = 0;
      const char** papszOptions = NULL; 
      OGRGeometry* good_geom
        = OGRGeometryFactory::organizePolygons(vw::geometry::vecPtr(ogr_polys),
                                               ogr_polys.size(),
                                               &pbIsValidGeometry,
                                               papszOptions);
    
      // Single polygon, nothing to do
      if (wkbFlatten(good_geom->getGeometryType()) == wkbPolygon || 
          wkbFlatten(good_geom->getGeometryType()) == wkbPoint) {
        bool append = false; 
        fromOGR(good_geom, poly_color, layer_str, polyVec, append);
      }else if (wkbFlatten(good_geom->getGeometryType()) == wkbMultiPolygon) {
      
        // We can merge
        OGRGeometry * merged_geom = new OGRPolygon;
      
        OGRMultiPolygon *poMultiPolygon = (OGRMultiPolygon*)good_geom;
      
        int numGeom = poMultiPolygon->getNumGeometries();
        for (int iGeom = 0; iGeom < numGeom; iGeom++){
        
          const OGRGeometry *currPolyGeom = poMultiPolygon->getGeometryRef(iGeom);
          if (wkbFlatten(currPolyGeom->getGeometryType()) != wkbPolygon) continue;
        
          OGRPolygon *poPolygon = (OGRPolygon *) currPolyGeom;
          OGRGeometry * local_merged = merged_geom->Union(poPolygon);
        
          // Keep the pointer to the new geometry
          if (merged_geom != NULL)
            OGRGeometryFactory::destroyGeometry(merged_geom);
          merged_geom = local_merged;
        }    
      
        bool append = false;
        fromOGR(merged_geom, poly_color, layer_str, polyVec, append);
        OGRGeometryFactory::destroyGeometry(merged_geom);
      }
    
      OGRGeometryFactory::destroyGeometry(good_geom);
       
    }catch(std::exception &e ){
      vw_out() << "OGR failed at " << e.what() << std::endl;
    }
    
    // Wipe all existing polygons and replace with this one
    for (size_t clipIter = 0; clipIter < imageData.size(); clipIter++) 
      imageData[clipIter].polyVec.clear();

    imageData[outIndex].polyVec = polyVec;
  }
  
  // Merge existing polygons
  void MainWidget::mergePolys(){
    MainWidget::mergePolys(m_images, m_polyLayerIndex);
  }
  
  // Save the currently created vector layer
  void MainWidget::saveVectorLayer(){
    
    // TODO: What if some images got deleted?
    if (m_polyLayerIndex >= int(m_images.size())){
      popUp("Images are inconsistent. Cannot save vector layer.");
      return;
    }
    
    std::string shapefile = m_images[m_polyLayerIndex].name;
    shapefile =  boost::filesystem::path(shapefile).replace_extension(".shp").string();
    QString qshapefile = QFileDialog::getSaveFileName(this,
                                                    tr("Save shapefile"), shapefile.c_str(),
                                                    tr("(*.shp)"));


    shapefile = qshapefile.toStdString();
    if (shapefile == "") 
      return;

    bool has_geo = m_images[m_polyLayerIndex].has_georef;
    vw::cartography::GeoReference const& geo = m_images[m_polyLayerIndex].georef;

    // TODO(oalexan1): What if there are polygons for many images?
    vw_out() << "Writing: " << shapefile << std::endl;
    write_shapefile(shapefile, has_geo, geo, m_images[m_polyLayerIndex].polyVec);
  }
  
  // Contour the current image
  bool MainWidget::contourImage(){
    
    int non_poly_image = -1;
    int num_non_poly_images = 0;
    int num_images = m_images.size();
    for (int image_iter = 0; image_iter < num_images; image_iter++) {
      if (!m_images[image_iter].isPoly())
        num_non_poly_images++;
      non_poly_image = image_iter;
    }

    if (num_non_poly_images > 1) {
      popUp("Must have just one image in window to contour an image.");
      return false;
    }

    if (non_poly_image < 0) 
      return true; // Will quietly skip this

    m_polyLayerIndex = non_poly_image;
    
    int num_channels = m_images[m_polyLayerIndex].img.planes();
    if (num_channels > 1) {
      popUp("Contouring images makes sense only for single-channel images.");
      return false;
    }
    
    if (num_channels == 1) 
      contour_image(m_images[m_polyLayerIndex].img, m_images[m_polyLayerIndex].georef,
                    m_thresh, m_images[m_polyLayerIndex].polyVec);
    
    // This will call paintEvent which will draw the contour
    update();
    
    return true;
  }
  
  void MainWidget::drawOneVertex(int x0, int y0, QColor color, int lineWidth,
                                 int drawVertIndex, QPainter &paint){

    // Draw a vertex as a small shape (a circle, rectangle, triangle)

    // Use variable size shapes to distinguish better points on top of
    // each other
    int len = 2*(drawVertIndex+1);
    len = std::min(len, 8); // limit how big this can get

    paint.setPen(QPen(color, lineWidth));

    int numTypes = 4;
    if (drawVertIndex < 0){

      // This will be reached only for the case when a polygon
      // is so small that it collapses into a point.
      len = lineWidth;
      paint.setBrush( color );
      paint.drawRect(x0 - len, y0 - len, 2*len, 2*len);

    } else if (drawVertIndex%numTypes == 0){

      // Draw a small empty ellipse
      paint.setBrush( Qt::NoBrush );
      paint.drawEllipse(x0 - len, y0 - len, 2*len, 2*len);

    }else if (drawVertIndex%numTypes == 1){

      // Draw an empty square
      paint.setBrush( Qt::NoBrush );
      paint.drawRect(x0 - len, y0 - len, 2*len, 2*len);

    }else if (drawVertIndex%numTypes == 2){

      // Draw an empty triangle
      paint.setBrush( Qt::NoBrush );
      paint.drawLine(x0 - len, y0 - len, x0 + len, y0 - len);
      paint.drawLine(x0 - len, y0 - len, x0 + 0,   y0 + len);
      paint.drawLine(x0 + len, y0 - len, x0 + 0,   y0 + len);

    }else{

      // Draw an empty reversed triangle
      paint.setBrush( Qt::NoBrush );
      paint.drawLine(x0 - len, y0 + len, x0 + len, y0 + len);
      paint.drawLine(x0 - len, y0 + len, x0 + 0,   y0 - len);
      paint.drawLine(x0 + len, y0 + len, x0 + 0,   y0 - len);

    }

    return;
  }
  
  void MainWidget::plotDPoly(bool plotPoints, bool plotEdges,
                             bool plotFilled, bool showIndices,
                             int lineWidth,
                             int drawVertIndex, // 0 is a good choice here
                             QColor const& color,
                             QPainter &paint,
                             vw::geometry::dPoly currPoly // Make a local copy on purpose
                             ){

    using namespace vw::geometry;

    if (m_world_box.empty()) return;
      
    // The box in world coordinates
    double x_min = m_world_box.min().x();
    double y_min = m_world_box.min().y();
    double x_max = m_world_box.max().x();
    double y_max = m_world_box.max().y();

    double screen_min_x = 0, screen_min_y = 0;

    // When polys are filled, plot largest polys first
    if (plotFilled) {
      // What on screen looks counter-clockwise, internally is clockwise,
      // because the screen y axis is in fact pointing down.
      // We need to reverse the orientation for the logic below to work properly
      //currPoly.reverse();  
      //currPoly.sortBySizeAndMaybeAddBigContainingRect(x_min,  y_min, x_max, y_max);
      //currPoly.reverse();
    }
    
    // Clip the polygon a bit beyond the viewing window, as to not see
    // the edges where the cut took place. It is a bit tricky to
    // decide how much the extra should be.
    double tol    = 1e-12;
    double pixelSize = std::max(m_world_box.width()/m_window_width,
                                m_world_box.height()/m_window_height);

    double extra  = 2*pixelSize*lineWidth;
    double extraX = extra + tol * std::max(std::abs(x_min), std::abs(x_max));
    double extraY = extra + tol * std::max(std::abs(y_min), std::abs(y_max));

    dPoly clippedPoly;
    currPoly.clipPoly(x_min - extraX, y_min - extraY,
		      x_max + extraX, y_max + extraY, // inputs
                      clippedPoly // output
                      );
    
    std::vector<vw::geometry::anno> annotations;
    if (showIndices) {
      clippedPoly.compVertIndexAnno();
      clippedPoly.get_vertIndexAnno(annotations);
    }
    
    const double * xv       = clippedPoly.get_xv();
    const double * yv       = clippedPoly.get_yv();
    const int    * numVerts = clippedPoly.get_numVerts();
    int numPolys            = clippedPoly.get_numPolys();

    // Aliases
    const std::vector<char> & isPolyClosed
      = clippedPoly.get_isPolyClosed();
    const std::vector<std::string> & colors
      = clippedPoly.get_colors(); // we ignore these

    int start = 0;
    for (int pIter = 0; pIter < numPolys; pIter++){

      if (pIter > 0) start += numVerts[pIter - 1];
      int pSize = numVerts[pIter];

      // Determine the orientation of polygons
      double signedArea = 0.0;
      if (plotFilled && isPolyClosed[pIter]){
        signedArea = signedPolyArea(pSize, xv + start, yv + start);
      }

      QPolygon pa(pSize);
      for (int vIter = 0; vIter < pSize; vIter++){

        Vector2 P = world2screen(Vector2(xv[start + vIter], yv[start + vIter]));
        pa[vIter] = QPoint(P.x(), P.y());

        // Qt's built in points are too small. Instead of drawing a point
        // draw a small shape.
        int tol = 4; // This is a bug fix for missing points. I don't understand
        //           // why this is necessary and why the number 4 is right.
        if ( plotPoints                                                                 &&
             P.x() > screen_min_x - tol && P.x() < screen_min_x + m_window_width  + tol &&
             P.y() > screen_min_y - tol && P.y() < screen_min_y + m_window_height + tol
             ){
          drawOneVertex(P.x(), P.y(), color, lineWidth, drawVertIndex, paint);
        }
      }

      if (pa.size() <= 0) continue;

      if (plotEdges){

        if (plotFilled && isPolyClosed[pIter]){
          // Notice that we fill clockwise polygons, those with negative area.
          // That because on screen they in fact appear counter-clockwise,
          // since the screen y axis is always down, and because
          // ESRI Shpefile format expects an outer polygon to be clockwise.
          if (signedArea < 0.0)  paint.setBrush( color );
          else                   paint.setBrush( m_backgroundColor );
          paint.setPen( Qt::NoPen );
        }else {
          paint.setBrush( Qt::NoBrush );
          paint.setPen( QPen(color, lineWidth) );
        }

        if ( isPolyZeroDim(pa) ){
          // Treat the case of polygons which are made up of just one point
          int l_drawVertIndex = -1;
          drawOneVertex(pa[0].x(), pa[0].y(), color, lineWidth, l_drawVertIndex,
                        paint);
        }else if (isPolyClosed[pIter]){

          if (plotFilled){
            paint.drawPolygon(pa);
          }else{
            // In some versions of Qt, drawPolygon is buggy when not
            // called to fill polygons. Don't use it, just draw the
            // edges one by one.
            int n = pa.size();
            for (int k = 0; k < n; k++){
              QPolygon pb;
              int x0, y0; pa.point(k, &x0, &y0);       pb << QPoint(x0, y0);
              int x1, y1; pa.point((k+1)%n, &x1, &y1); pb << QPoint(x1, y1);
              paint.drawPolyline(pb);
            }
          }

        }else{
          paint.drawPolyline(pa); // don't join the last vertex to the first
        }

      }
    }

    // Plot the annotations
    int numAnno = annotations.size();
    for (int aIter = 0; aIter < numAnno; aIter++){
      const anno & A = annotations[aIter];
      // Avoid points close to boundary, as were we clipped artificially
      if (! (A.x >= x_min && A.x <= x_max && A.y >= y_min && A.y <= y_max ) ) continue;
      Vector2 P = world2screen(Vector2(A.x, A.y));
      paint.setPen( QPen(QColor("gold"), lineWidth) );
      paint.drawText(P.x(), P.y(), (A.label).c_str());
    } // End plotting annotations
    
    return;
  }
  
  
  // Go to the pixel locations on screen, and draw the polygonal line.
  // This is robust to zooming in the middle of profiling.
  // TODO: This will function badly when zooming.
  void MainWidget::plotProfilePolyLine(QPainter & paint,
                                       std::vector<double> const& profileX, 
                                       std::vector<double> const& profileY){

    if (profileX.empty()) return;

    paint.setPen(QColor("red"));
    std::vector<QPoint> profilePixels;
    for (size_t it = 0; it < profileX.size(); it++) {
      Vector2 P = world2screen(Vector2(profileX[it], profileY[it]));
      QPoint Q(P.x(), P.y());
      paint.drawEllipse(Q, 2, 2); // Draw the point, and make it a little large
      profilePixels.push_back(Q);
    }
    paint.drawPolyline(&profilePixels[0], profilePixels.size());
  }

  
  void MainWidget::mousePressEvent(QMouseEvent *event) {

    // for rubberband
    m_mousePrsX  = event->pos().x();
    m_mousePrsY  = event->pos().y();
    m_rubberBand = m_emptyRubberBand;

    m_curr_pixel_pos = QPoint2Vec(QPoint(m_mousePrsX, m_mousePrsY)); // Record where we clicked
    m_last_gain      = m_gain;   // Store this so the user can do linear
    m_last_offset    = m_offset; // and nonlinear steps.
    m_last_gamma     = m_gamma;
    updateCurrentMousePosition();

    // Need this for panning
    m_last_view = m_current_view;

    // Check if the user is holding down the crop window key.
    m_cropWinMode = ( (event->buttons  () & Qt::LeftButton     ) &&
                      (event->modifiers() & Qt::ControlModifier) );

    m_editMatchPointVecIndex = -1; // Keep this initialized

    // If the user is currently editing match points
    if (!m_polyEditMode && m_moveMatchPoint->isChecked()
        && !m_cropWinMode && m_view_matches){

      m_editingMatches = true;
      
      size_t  trans_image_id = getTransformImageIndex();
      Vector2 P = screen2world(Vector2(m_mousePrsX, m_mousePrsY));
      P = world2image(P, trans_image_id);

      // Find the match point we want to move
      const double DISTANCE_LIMIT = 70;
      m_editMatchPointVecIndex = m_matchlist.findNearestMatchPoint(m_image_id, P, DISTANCE_LIMIT);

      emit turnOnViewMatchesSignal(); // Update IP draw color
    } // End match point update case

    // If the user is currently editing polygons
    if (m_polyEditMode && m_moveVertex->isChecked() && !m_cropWinMode){

      // Ensure these are always initialized
      m_editPolyVecIndex        = -1;
      m_editIndexInCurrPoly     = -1;
      m_editVertIndexInCurrPoly = -1;

      Vector2 P = screen2world(Vector2(m_mousePrsX, m_mousePrsY));
      m_world_box.grow(P); // to not cut when plotting later
      
      // Find the vertex we want to move
      double min_x, min_y, min_dist;
      MainWidget::findClosestPolyVertex(// inputs
                                        P.x(), P.y(), m_images,
                                        // outputs
                                        m_polyLayerIndex,
                                        m_editPolyVecIndex,
                                        m_editIndexInCurrPoly,
                                        m_editVertIndexInCurrPoly,
                                        min_x, min_y, min_dist);

      // When all the polygons are empty, make sure that at least
      // m_polyLayerIndex is valid.
      if (m_polyLayerIndex < 0) 
        m_polyLayerIndex = 0;
      
      // This will redraw just the polygons, not the pixmap
      update();

      // The action will continue in mouseMoveEvent()
      
    } // End polygon update case

  } // End function mousePressEvent()

  void MainWidget::mouseMoveEvent(QMouseEvent *event) {

    QPoint Q = event->pos();
    int mouseMoveX = Q.x(), mouseMoveY = Q.y();
    
    m_curr_pixel_pos = QPoint2Vec(event->pos());
    updateCurrentMousePosition();

    if (!((event->buttons() & Qt::LeftButton)))
      return;

    // The mouse is pressed and moving

    m_cropWinMode = ( (event->buttons  () & Qt::LeftButton) &&
                      (event->modifiers() & Qt::ControlModifier) );

    // If the user is editing match points
    if (!m_polyEditMode && m_moveMatchPoint->isChecked() && !m_cropWinMode){

      m_editingMatches = true;

      // Error checking
      if ( (m_image_id < 0) || (m_editMatchPointVecIndex < 0) ||
           (!m_matchlist.pointExists(m_image_id, m_editMatchPointVecIndex)) )
        return;

      size_t  trans_image_id = getTransformImageIndex();
      Vector2 P = screen2world(Vector2(mouseMoveX, mouseMoveY));
      P = world2image(P, trans_image_id);

      // Update the IP location
      m_matchlist.setPointPosition(m_image_id, m_editMatchPointVecIndex, P.x(), P.y());

      emit turnOnViewMatchesSignal(); // Update IP draw color
      return;
    } // End polygon editing

    // If the user is editing polygons
    if (m_polyEditMode && m_moveVertex->isChecked() && !m_cropWinMode){

      // If moving vertices
      if (m_editPolyVecIndex        < 0 ||
          m_editIndexInCurrPoly     < 0 ||
          m_editVertIndexInCurrPoly < 0)
        return;

      Vector2 P = screen2world(Vector2(mouseMoveX, mouseMoveY));

      m_world_box.grow(P); // to not cut when plotting later
      P = world2projpoint(P, m_polyLayerIndex); // projected units
      m_images[m_polyLayerIndex].polyVec[m_editPolyVecIndex].changeVertexValue(m_editIndexInCurrPoly,
                                                                             m_editVertIndexInCurrPoly,
                                                                             P.x(), P.y());
      // This will redraw just the polygons, not the pixmap
      update();
      return;
    } // End polygon editing

    // Standard Qt rubberband trick. This is highly confusing.  The
    // explanation for what is going on is the following.  We need to
    // wipe the old rubberband, and draw a new one.  Hence just the
    // perimeters of these two rectangles need to be re-painted,
    // nothing else changes. The first updateRubberBand() call below
    // schedules that the perimeter of the current rubberband be
    // repainted, but the actual repainting, and this is the key, WILL
    // HAPPEN LATER! Then we change m_rubberBand to the new value,
    // then we schedule the repaint event on the new rubberband.
    // Continued below.
    updateRubberBand(m_rubberBand);
    m_rubberBand = QRect( std::min(m_mousePrsX, mouseMoveX),
                          std::min(m_mousePrsY, mouseMoveY),
                          std::abs(mouseMoveX - m_mousePrsX),
                          std::abs(mouseMoveY - m_mousePrsY) );
    updateRubberBand(m_rubberBand);
    // Only now, a single call to MainWidget::PaintEvent() happens,
    // even though it appears from above that two calls could happen
    // since we requested two updates. This call updates the perimeter
    // of the old rubberband, in effect wiping it, since the region
    // occupied by the old rubberband is scheduled to be repainted,
    // but the rubberband itself is already changed.  It also updates
    // the perimeter of the new rubberband, and as can be seen in
    // MainWidget::PaintEvent() the effect is to draw the rubberband.
	
    if (m_cropWinMode && !m_allowMultipleSelections) {
      // If there is on screen already a crop window, wipe it, as
      // we are now in the process of creating a new one.
      QRect R = bbox2qrect(world2screen(m_stereoCropWin));
      updateRubberBand(R);
      m_stereoCropWin = BBox2();
      R = bbox2qrect(world2screen(m_stereoCropWin));
      updateRubberBand(R);
    }

    return;
  } // End function mouseMoveEvent()

// TODO: Clean up this monster function!
  void MainWidget::mouseReleaseEvent (QMouseEvent *event){

    QPoint mouse_rel_pos = event->pos();
    int mouseRelX = mouse_rel_pos.x(),
        mouseRelY = mouse_rel_pos.y();

    if( (event->buttons  () & Qt::LeftButton) &&
        (event->modifiers() & Qt::ControlModifier) ){
      m_cropWinMode = true;
    }

    if (m_images.empty())
      return;

    // If a point was being moved, reset the ID and color.
    // - Points that are moved are also set to valid.
    if (m_editMatchPointVecIndex >= 0) {
      m_matchlist.setPointValid(m_image_id, m_editMatchPointVecIndex, true);
      m_editMatchPointVecIndex = -1;
      emit turnOnViewMatchesSignal(); // Update IP draw color
    }

    // If the mouse was released close to where it was pressed
    if (std::abs(m_mousePrsX - mouseRelX) < m_pixelTol &&
        std::abs(m_mousePrsY - mouseRelY) < m_pixelTol ) {

      if (!m_thresh_calc_mode){

        Vector2 p = screen2world(Vector2(mouseRelX, mouseRelY));
        
        if (!m_profileMode && !m_polyEditMode) {
          QPainter paint(&m_pixmap);
          paint.initFrom(this);
          QPoint Q(mouseRelX, mouseRelY);
          paint.setPen(QColor("red"));
          paint.drawEllipse(Q, 2, 2); // Draw the point, and make it a little large
        }
        
        bool can_profile = m_profileMode;
        
        // Print pixel coordinates and image value.
        for (size_t j = 0; j < m_images.size(); j++){
          
          int it = m_filesOrder[j];
          
          // Don't show files the user wants hidden
          std::string fileName = m_images[it].name;
          if (m_filesToHide.find(fileName) != m_filesToHide.end())
            continue;
          
          std::string val = "none";
          Vector2 q = world2image(p, it);
          
          int col = floor(q[0]), row = floor(q[1]);
          
          if (col >= 0 && row >= 0 && col < m_images[it].img.cols() &&
              row < m_images[it].img.rows() ) {
            val = m_images[it].img.get_value_as_str(col, row);
          }

          vw_out() << "Pixel and value for " << m_images[it].name << ": "
                   << col << ' ' << row << ' ' << val << std::endl;

          update();

          if (m_profileMode) {

            // Sanity checks
            if (m_images.size() != 1) {
              popUp("A profile can be shown only when a single image is present.");
              can_profile = false;
            }
            int num_channels = m_images[it].img.planes();
            if (num_channels != 1) {
              popUp("A profile can be shown only when the image has a single channel.");
              can_profile = false;
            }

            if (!can_profile) {
              MainWidget::setProfileMode(can_profile);
              return;
            }

          } // End if m_profileMode

        } // end iterating over images

        if (can_profile) {
          // Save the current point the user clicked onto in the
          // world coordinate system.
          m_profileX.push_back(p.x());
          m_profileY.push_back(p.y());

          // PaintEvent() will be called, which will call
          // plotProfilePolyLine() to show the polygonal line.

          // Now show the profile.
          MainWidget::plotProfile(m_images, m_profileX, m_profileY);

          // TODO: Why is this buried in the short distance check?
        } else if (m_polyEditMode && m_moveVertex->isChecked() && !m_cropWinMode){
          // Move vertex

          if (m_editPolyVecIndex        < 0 ||
              m_editIndexInCurrPoly     < 0 ||
              m_editVertIndexInCurrPoly < 0)
            return;

          Vector2 P = screen2world(Vector2(mouseRelX, mouseRelY));
          m_world_box.grow(P); // to not cut when plotting later
          P = world2projpoint(P, m_polyLayerIndex); // projected units
          m_images[m_polyLayerIndex].polyVec[m_editPolyVecIndex].changeVertexValue(m_editIndexInCurrPoly,
                                                          m_editVertIndexInCurrPoly,
                                                          P.x(), P.y());

          // These are no longer needed for the time being
          m_editPolyVecIndex        = -1;
          m_editIndexInCurrPoly     = -1;
          m_editVertIndexInCurrPoly = -1;

          // This will redraw just the polygons, not the pixmap
          update();

        } else if (m_polyEditMode) {
          // Add vertex
          addPolyVert(mouseRelX, mouseRelY);
        }

      }else{
        // Image threshold mode. If we released the mouse where we
        // pressed it, that means we want the current pixel value
        // to be the threshold if larger than the existing threshold.
        if (m_images.size() != 1) {
          popUp("Must have just one image in each window to do image threshold detection.");
          m_thresh_calc_mode = false;
          refreshPixmap();
          return;
        }

        if (m_images[0].img.planes() != 1) {
          popUp("Thresholding makes sense only for single-channel images.");
          m_thresh_calc_mode = false;
          return;
        }
        
        if (m_use_georef) {
          popUp("Thresholding is not supported when using georeference information to show images.");
          m_thresh_calc_mode = false;
          return;
        }

        Vector2 p = screen2world(Vector2(mouseRelX, mouseRelY));
        Vector2 q = world2image(p, 0);

        int col = round(q[0]), row = round(q[1]);
        vw_out() << "Clicked on pixel: " << col << ' ' << row << std::endl;

        if (col >= 0 && row >= 0 && col < m_images[0].img.cols() &&
            row < m_images[0].img.rows() ) {
          double val      = m_images[0].img.get_value_as_double(col, row);
          m_thresh = std::max(m_thresh, val);
        }

        vw_out() << "Image threshold for "
                 << m_images[0].name
                 << ": " << m_thresh << std::endl;
        return;
      }

      return;
    } // end the case when the mouse was released close to where it was pressed

    // Do not zoom or do other funny stuff if we are moving IP or vertices
    if (!m_polyEditMode && m_moveMatchPoint->isChecked() && !m_cropWinMode)
      return;
    if (m_polyEditMode && m_moveVertex->isChecked() && !m_cropWinMode)
      return;

    if (event->buttons() & Qt::RightButton) {
      // Drag the image along the mouse movement
      m_current_view -= (screen2world(QPoint2Vec(mouse_rel_pos)) -
                         screen2world(QPoint2Vec(QPoint(m_mousePrsX, m_mousePrsY))));

      refreshPixmap(); // will call paintEvent()

    } else if (m_cropWinMode){

      // If now we allow multiple selected regions, but we did not allow at
      // the time the crop win was formed, save the crop win before it
      // will be overwritten.
      if (m_allowMultipleSelections && !m_stereoCropWin.empty()) {
        if (m_selectionRectangles.empty() ||
            m_selectionRectangles.back() != m_stereoCropWin) {
          m_selectionRectangles.push_back(m_stereoCropWin);
        }
      }
      
      // User selects the region to use for stereo.  Convert it to world
      // coordinates, and round to integer.  If we use georeferences,
      // the crop win is in projected units for the first image,
      // so we must convert to pixels.
      m_stereoCropWin = screen2world(qrect2bbox(m_rubberBand));

      if (m_allowMultipleSelections && !m_stereoCropWin.empty()) {
        m_selectionRectangles.push_back(m_stereoCropWin);
      }
      
      for (int j = 0; j < (int)m_images.size(); j++){
        
        int image_it = m_filesOrder[j];
        
        // Don't show files the user wants hidden
        std::string fileName = m_images[image_it].name;
        if (m_filesToHide.find(fileName) != m_filesToHide.end()) continue;

        BBox2 image_box = world2image(m_stereoCropWin, image_it); 
        vw_out().precision(8);
        vw_out() << "Crop src win for  "
                 << m_images[image_it].name
                 << ": "
                 << round(image_box.min().x()) << ' '
                 << round(image_box.min().y()) << ' '
                 << round(image_box.width())   << ' '
                 << round(image_box.height())  << std::endl;

        if (m_images[image_it].has_georef){
          Vector2 proj_min, proj_max;
          // Convert pixels to projected coordinates
          BBox2 point_box;
          if (m_images[image_it].isPoly())
            point_box = image_box;
          else 
            point_box = m_images[image_it].georef.pixel_to_point_bbox(image_box);

          proj_min = point_box.min();
          proj_max = point_box.max();
          // Below we flip in y to make gdal happy
          vw_out() << "Crop proj win for "
                   << m_images[image_it].name << ": "
                   << proj_min.x() << ' ' << proj_max.y() << ' '
                   << proj_max.x() << ' ' << proj_min.y() << std::endl;

          Vector2 lonlat_min, lonlat_max;
          BBox2 lonlat_box = m_images[image_it].georef.point_to_lonlat_bbox(point_box);
          lonlat_min = lonlat_box.min();
          lonlat_max = lonlat_box.max();
          // Again, miny and maxy are flipped on purpose
          vw_out() << "lonlat win for    "
                   << m_images[image_it].name << ": "
                   << lonlat_min.x() << ' ' << lonlat_max.y() << ' '
                   << lonlat_max.x() << ' ' << lonlat_min.y() << std::endl;
        }
      }

      // Wipe the rubberband, no longer needed.
      updateRubberBand(m_rubberBand);
      m_rubberBand = m_emptyRubberBand;
      updateRubberBand(m_rubberBand);

      // Draw the crop window. This may not be precisely the rubberband
      // since there is some loss of precision in conversion from
      // Qrect to BBox2 and back. Note actually that we are not drawing
      // here, we are scheduling this area to be updated, the drawing
      // has to happen (with precisely this formula) in PaintEvent().
      QRect R = bbox2qrect(world2screen(m_stereoCropWin));
      updateRubberBand(R);

    } else if (Qt::LeftButton) {

      // Zoom

      // Wipe the rubberband
      updateRubberBand(m_rubberBand);
      m_rubberBand = m_emptyRubberBand;
      updateRubberBand(m_rubberBand);

      m_can_emit_zoom_all_signal = true; 
        
      if (mouseRelX > m_mousePrsX && mouseRelY > m_mousePrsY) {

        // Dragging the mouse from upper-left to lower-right zooms in

        // The window selected with the mouse in world coordinates
        Vector2 A = screen2world(Vector2(m_mousePrsX, m_mousePrsY));
        Vector2 B = screen2world(Vector2(mouseRelX, mouseRelY));
        BBox2 view = BBox2(A, B);

        // Zoom to this window. Don't zoom so much that the view box
        // ends up having size 0 to numerical precision.
        if (!view.empty())
          m_current_view = expand_box_to_keep_aspect_ratio(view);

        // Must redraw the entire image
        refreshPixmap();

      } else if (mouseRelX < m_mousePrsX && mouseRelY < m_mousePrsY) {
        // Dragging the mouse in reverse zooms out
        double scale = 0.8;
        zoom(scale);
      }

    }
    
    // At this stage the user is supposed to release the control key, so
    // we are no longer in crop win mode, even if we were so far.
    m_cropWinMode = false;

    return;
  } // End mouseReleaseEvent()

  void MainWidget::mouseDoubleClickEvent(QMouseEvent *event) {
    m_curr_pixel_pos = QPoint2Vec(event->pos());
    updateCurrentMousePosition();
  }

  void MainWidget::wheelEvent(QWheelEvent *event) {
    int num_degrees = event->delta();
    double num_ticks = double(num_degrees) / 360;

    // 2.0 chosen arbitrarily here as a reasonable scale factor giving good
    // sensitivity of the mousewheel. Shift zooms 50 times slower.
    double scale_factor = 2;
    if (event->modifiers() & Qt::ShiftModifier)
      scale_factor *= 50;

    double mag = std::abs(num_ticks/scale_factor);
    double scale = 1;
    if (num_ticks > 0)
      scale = 1+mag;
    else if (num_ticks < 0)
      scale = 1-mag;

    zoom(scale);

    m_curr_pixel_pos = QPoint2Vec(event->pos());
    updateCurrentMousePosition();
  }


  void MainWidget::enterEvent(QEvent *event) {
  }

  void MainWidget::leaveEvent(QEvent */*event*/) {
  }

  void MainWidget::keyPressEvent(QKeyEvent *event) {

    std::ostringstream s;

    // Save these before we modify the box
    double width  = m_current_view.width();
    double height = m_current_view.height();
      
    double factor = 0.2;  // We will pan by moving by 20%.
    switch (event->key()) {

      // Pan
    case Qt::Key_Left: // Pan left
      m_current_view.min().x() -= width*factor;
      m_current_view.max().x() -= width*factor;
      m_can_emit_zoom_all_signal = true;
      refreshPixmap();
      break;
    case Qt::Key_Right: // Pan right
      m_current_view.min().x() += width*factor;
      m_current_view.max().x() += width*factor;
      m_can_emit_zoom_all_signal = true;
      refreshPixmap();
      break;
    case Qt::Key_Up: // Pan up
      m_current_view.min().y() -= height*factor;
      m_current_view.max().y() -= height*factor;
      m_can_emit_zoom_all_signal = true;
      refreshPixmap();
      break;
    case Qt::Key_Down: // Pan down
      m_current_view.min().y() += height*factor;
      m_current_view.max().y() += height*factor;
      m_can_emit_zoom_all_signal = true;
      refreshPixmap();
      break;

      // Zoom out
    case Qt::Key_Minus:
    case Qt::Key_Underscore:
      zoom(0.75);
      break;

      // Zoom in
    case Qt::Key_Plus:
    case Qt::Key_Equal:
      zoom(1.0/0.75);
      break;

    default:
      QWidget::keyPressEvent(event);
    }
  }

  void MainWidget::contextMenuEvent(QContextMenuEvent *event){

    int x = event->x(), y = event->y();
    m_mousePrsX = x;
    m_mousePrsY = y;

    // If in poly edit mode, turn on these items.
    m_deleteVertex->setVisible(m_polyEditMode);
    m_deleteVertices->setVisible(m_polyEditMode);
    m_insertVertex->setVisible(m_polyEditMode);
    m_moveVertex->setVisible(m_polyEditMode);
    m_showIndices->setVisible(m_polyEditMode);
    m_showPolysFilled->setVisible(m_polyEditMode);

    // Add the saving polygon option even when not editing
    m_saveVectorLayer->setVisible(true);
    
    m_mergePolys->setVisible(m_polyEditMode);
    
    // Refresh this from the variable, before popping up the menu
    m_allowMultipleSelections_action->setChecked(m_allowMultipleSelections);

    // Turn on these items if we are NOT in poly edit mode.
    m_addMatchPoint->setVisible(!m_polyEditMode); 
    m_deleteMatchPoint->setVisible(!m_polyEditMode); 
    m_moveMatchPoint->setVisible(!m_polyEditMode);
    m_toggleHillshade->setVisible(!m_polyEditMode); 
    m_setHillshadeParams->setVisible(!m_polyEditMode); 
    m_setThreshold->setVisible(!m_polyEditMode); 
    m_allowMultipleSelections_action->setVisible(!m_polyEditMode); 
    m_deleteSelection->setVisible(true);
    m_hideImagesNotInRegion->setVisible(true);
    
    m_saveScreenshot->setVisible(true); // always visible
    
    m_ContextMenu->popup(mapToGlobal(QPoint(x,y)));
    return;
  }

  void MainWidget::viewMatches(bool view_matches){
    // Complain if there are multiple images and matches was turned on
    if ((m_images.size() != 1) && view_matches) {
      emit turnOffViewMatchesSignal();
      return;
    }

    m_view_matches = view_matches;
    update(); // redraw matches on top of existing images
  }

  
  void MainWidget::addMatchPoint(){

    if (m_image_id >= static_cast<int>(m_matchlist.getNumImages())) {
      popUp("Number of existing matches is corrupted. Cannot add matches.");
      return;
    }

    if (m_images.size() != 1) {
      emit turnOffViewMatchesSignal();
      return;
    }

    m_editingMatches = true;

    // Convert mouse coords to world coords then image coords.
    size_t  trans_image_id = getTransformImageIndex();
    Vector2 world_coord    = screen2world(Vector2(m_mousePrsX, m_mousePrsY));
    Vector2 P              = world2image(world_coord, trans_image_id);

    // Try to add the new IP.
    bool is_good = m_matchlist.addPoint(m_image_id, ip::InterestPoint(P.x(), P.y()));

    if (!is_good) {
      popUp(std::string("Add matches by adding a point in the left-most ")
            + "image and corresponding matches in the other images left to right. "
            + "Cannot add this match.");
      return;
    }

    // Must refresh the matches in all the images, not just this one
    emit turnOnViewMatchesSignal();
  }

  // We cannot delete match points unless all images have the same number of them.
  void MainWidget::deleteMatchPoint(){

    if (m_images.size() != 1) {
      popUp("Must have just one image in each window to delete matches.");
      return;
    }

    if (m_matchlist.getNumPoints() == 0){
      popUp("No matches to delete.");
      return;
    }

    // Find the closest match to this point.
    size_t trans_image_id = getTransformImageIndex();
    Vector2 P = screen2world(Vector2(m_mousePrsX, m_mousePrsY));
    P = world2image(P, trans_image_id);
    const double DISTANCE_LIMIT = 70;
    int min_index = m_matchlist.findNearestMatchPoint(m_image_id, P, DISTANCE_LIMIT);
    if (min_index < 0) {
      popUp("Did not find a nearby match to delete.");
      return;
    }

    m_editingMatches = true;

    bool result = m_matchlist.deletePointAcrossImages(min_index);
    
    if (result) {
      // Must refresh the matches in all the images, not just this one
      emit turnOnViewMatchesSignal();
    }
  }

  // Delete the selections that contain the current point
  void MainWidget::deleteSelection(){

    Vector2 P = screen2world(Vector2(m_mousePrsX, m_mousePrsY));

    // The main crop win
    if (m_stereoCropWin.contains(P)){
      QRect R = bbox2qrect(world2screen(m_stereoCropWin));
      updateRubberBand(R); // mark that later we should redraw this polygonal line
      m_stereoCropWin = BBox2();
    }

    std::vector<BBox2> curr_rects;
    for (size_t it = 0; it < m_selectionRectangles.size(); it++) {
      if (!m_selectionRectangles[it].contains(P)) {
        curr_rects.push_back(m_selectionRectangles[it]);
      }else{
        QRect R = bbox2qrect(world2screen(m_selectionRectangles[it]));
        updateRubberBand(R); // mark that later we should redraw this polygonal line
      }
    }
    m_selectionRectangles = curr_rects;

    return;
  }
  
  // Hide images not intersecting given selected region
  void MainWidget::hideImagesNotInRegion(){

    if (m_stereoCropWin.empty()) {
      popUp("Must select a region with Control-Mouse before invoking this.");
      return;
    }
    
    m_filesToHide.clear();

    QTableWidget * filesTable = m_chooseFilesDlg->getFilesTable();

    for (int j = 0; j < (int)m_images.size(); j++){
      
      int image_it = m_filesOrder[j];
      
      std::string fileName = m_images[image_it].name;
      
      BBox2i image_box = world2image(m_stereoCropWin, image_it);
      image_box.crop(BBox2(0, 0, m_images[image_it].img.cols(), m_images[image_it].img.rows()));

      QTableWidgetItem *item = filesTable->item(image_it, 0);

      if (image_box.empty()) {
        item->setCheckState(Qt::Unchecked);
        m_filesToHide.insert(fileName);
      }else{
        item->setCheckState(Qt::Checked);
      }	
      
    }
    
    refreshPixmap();

    return;
  }

  // Show the current image threshold, and allow the user to change it.
  void MainWidget::setThreshold(){

    std::ostringstream oss;
    oss.precision(18);
    oss << m_thresh;
    std::string imageThresh = oss.str();
    bool ans = getStringFromGui(this,
				"Image threshold",
				"Image threshold",
				imageThresh,
				imageThresh);
    if (!ans)
      return;

    double thresh = atof(imageThresh.c_str());
    MainWidget::setThreshold(thresh);
  }

  void MainWidget::setThreshold(double thresh){

    int non_poly_image = 0;
    int num_non_poly_images = 0;
    int num_images = m_images.size();
    for (int image_iter = 0; image_iter < num_images; image_iter++) {
      if (!m_images[image_iter].isPoly())
        num_non_poly_images++;
      non_poly_image = image_iter;
    }
    
    if (num_non_poly_images > 1){
      if (std::isnan(asp::stereo_settings().nodata_value)) 
        popUp("Must have just one image in each window to set the image threshold.");
      else
        popUp("Must have just one image in each window to use the nodata value option.");
      return;
    }

    m_thresh = thresh;
    vw_out() << "Image threshold for " << m_images[non_poly_image].name
	     << ": " << m_thresh << std::endl;
  }

  double MainWidget::getThreshold(){
    return m_thresh;
  }

  void MainWidget::setPolyColor(std::string const& polyColor){
    m_polyColor = polyColor;
    update();
  }
  
  std::string MainWidget::getPolyColor() {
    return m_polyColor;
  }
  
  void MainWidget::setLineWidth(int lineWidth){
    m_lineWidth = lineWidth;
    update();
  }

  int MainWidget::getLineWidth() {
    return m_lineWidth;
  }
  
  // Set the azimuth and elevation for hillshaded images
  void MainWidget::setHillshadeParams(){

    std::ostringstream oss;
    oss.precision(18);
    oss << m_hillshade_azimuth
        << " "
        << m_hillshade_elevation << std::endl;

    std::string azimuthElevation = oss.str();
    bool ans = getStringFromGui(this,
				"Hillshade azimuth and elevation",
				"Hillshade azimuth and elevation",
				azimuthElevation,
				azimuthElevation);
    if (!ans)
      return;

    std::istringstream iss(azimuthElevation);
    double a, e; 
    if (! (iss >> a >> e) ) {
      popUp("Could not read the hillshade azimuth and elevation values.");
      return;
    }
    m_hillshade_azimuth = a;
    m_hillshade_elevation = e;

    MainWidget::maybeGenHillshade();
    refreshPixmap();

    vw_out() << "Hillshade azimuth and elevation for " << m_images[0].name
	     << ": "
             << m_hillshade_azimuth << ' '
             << m_hillshade_elevation << std::endl;
      
  }

  // Save the current view to a file
  void MainWidget::saveScreenshot(){

    QString fileName = QFileDialog::getSaveFileName(this,
                                                    tr("Save screenshot"), "./screenshot.bmp",
                                                    tr("(*.bmp *.xpm)"));

    if (fileName.toStdString() == "") 
      return;

    QImageWriter writer(fileName);
    if(!writer.write(m_pixmap.toImage())){
      popUp(writer.errorString().toStdString());
    }
    
  }
  
}} // namespace vw::gui
