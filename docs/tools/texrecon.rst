.. _texrecon:

texrecon
--------

The ``texrecon`` script takes as input several images, their camera
poses, camera intrinsics, and a mesh, for one or ore sensors, and
creates a textured mesh as an .obj file. This tool is a wrapper around
the third-party `MVS-Texturing
<https://github.com/nmoehrle/mvs-texturing>`_ software.

Example
^^^^^^^

The dataset:

    https://github.com/NeoGeographyToolkit/StereoPipelineSolvedExamples/releases/tag/rig_calibrator

has an example of how a set of camera images acquired with a rig were
co-registered using the ``rig_calibrator`` program
(:numref:`rig_calibrator`), and how a mesh was created.

With that data and this tool, a textured mesh can be obtained as follows::

    texrecon --rig_config rig_out/rig_config.txt \
      --camera_poses rig_out/cameras.txt         \
      --mesh rig_out/fused_mesh.ply              \
      --rig_sensor sci_cam                       \
      --undistorted_crop_win '1000 800'          \
      --out_dir texrecon_out

The inputs to this program need not be created with
``rig_calibrator``. What is important is that the camera poses and the
mesh be co-registered.

Command-line options
^^^^^^^^^^^^^^^^^^^^

--rig_config <string>
   Rig configuration file.
--rig_sensor <string>
   Which rig sensor images to texture. Can be more than one. Must be
   among the sensors specified via ``--rig_config``.
--camera_poses <string>
   Read images and camera poses from this list.
--mesh <string>
   The mesh to use for texturing, in .ply format.
--undistorted_crop_win <string>
   The dimensions of the central image region to keep
   after undistorting an image and before using it in texturing.
   Normally 85% - 90% of distorted (actual) image
   dimensions would do. Suggested for Astrobee images: sci_cam: '1250
   1000' nav_cam: '1100 776'. haz_cam: '250 200'.
--max_texture_size <int>
   The maximum size (in pixels) of each texture file created for the
   produced textured mesh.
--out_dir <string>
   The directory where to write the textured mesh and
   other data.
--skip_local_seam_leveling
   If set, skip a postprocesing algorithm which may remove some seams
   but which on occasion can cause a crash.
