AD7091R IIO Application
"""""""""""""""""""""""

==================
Supported Hardware
==================

**Supported Devices:**

* `AD7091R-8 <https://www.analog.com/en/products/ad7091r-8.html>`_ 

**Supported Evaluation Boards:**

* `EVAL-AD7091R-8ARDZ <https://www.analog.com/en/resources/evaluation-hardware-and-software/evaluation-boards-kits/eval-ad7091r.html>`_ 

**Supported Carrier Boards:**

* `SDP-K1 With Mbed <https://os.mbed.com/platforms/SDP_K1/>`_

============
Introduction
============

This page gives an overview of using the ARM platforms supported (default is Mbed) 
firmware example with Analog Devices AD7091r Evaluation board and SDP-K1 controller board. 
This example code leverages the ADI developed IIO (Industrial Input Output) ecosystem to 
evaluate the AD7091r device by providing a device debug and data capture support.

   .. image:: /source/tinyiiod/app_interface.png
      :width: 350

The interface used for communicating with PC based IIO clients is either Virtual Serial Or UART. 
IIO Firmware leverages the ADI created no-os and platform driver software layers
to communicates with IIO device.

.. SDP-K1 Mbed Section

.. include:: /source/tinyiiod/sdp_k1_mbed.rst

.. Useful links Section

.. include:: /source/useful_links.rst

====================
Hardware Connections
====================

Required: SDP-K1 (or alternative Mbed enabled controller board), EVAL-AD7091R-8ARDZ board
and USB cable.

Connect the EVAL-AD7091R-8ARDZ board to SDP-K1 board (or any other Mbed enabled controller 
board). Connect controller board to the PC using the USB cable. 


   .. image:: /source/projects/ad7091r_iio/ad7091r_hardware_interface.png
      :width: 600


===============
Jumper Settings
===============

**SDP-K1:**

Connect the VIO_ADJUST jumper on the SDP-K1 board to 3.3V position to drive SDP-K1 GPIOs at 3.3V

**EVAL-AD7091R:**

* Please refer to the respective board user guide on the product page of the chosen device.

.. note::
   Anything?

.. Communication Interface section:

.. include:: /source/hardware/comm_interface.rst

.. Project Build Section:
    
.. include:: /source/build/project_build.rst

.. IIO Ecosystem Section:
    
.. include:: /source/tinyiiod/iio_ecosystem.rst

.. IIO Firmware Structure

.. include:: /source/tinyiiod/iio_firmware_structure.rst

=======
Support
=======

Feel free to ask questions in the `EngineerZone <https://ez.analog.com/data_converters>`_
