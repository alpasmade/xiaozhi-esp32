# Waveshare ESP32-S3-Touch-AMOLED-1.75


[ESP32-S3-Touch-AMOLED-1.75](https://www.waveshare.com/esp32-s3-touch-amoled-1.75.htm) ESP32-S3-Touch-AMOLED-1.75 is a high performance, highly integrated microcontroller development board designed by Waveshare.\
In the smaller form, the 1.75-inch capacitive HD AMOLED screen, highly integrated power management chip, six-axis sensor (three-axis accelerometer and three-axis gyroscope), RTC, low-power audio codec chip and echo cancellation circuit are mounted on the board, which is convenient for development and embedding into the product.


## images

| [ESP32-S3-Touch-AMOLED-1.75](https://www.waveshare.com/esp32-s3-touch-amoled-1.75.htm)                                                                                                                                                                                           |
|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| <img style="width: 150px; height: auto; display: block; margin: 0 auto;" src= "https://www.waveshare.com/media/catalog/product/cache/1/image/800x800/9df78eab33525d08d6e5fb8d27136e95/e/s/esp32-s3-touch-amoled-1.75-1.jpg"> |
| <img style="width: 150px; height: auto; display: block; margin: 0 auto;" src= "https://www.waveshare.com/media/catalog/product/cache/1/image/560x560/9df78eab33525d08d6e5fb8d27136e95/e/s/esp32-s3-touch-amoled-1.75-b-3.jpg"> |
| <img style="width: 150px; height: auto; display: block; margin: 0 auto;" src= "https://www.waveshare.com/media/catalog/product/cache/1/image/560x560/9df78eab33525d08d6e5fb8d27136e95/e/s/esp32-s3-touch-amoled-1.75-b-1.jpg"> |


Jakomotchi: Jako's face (jako_face.c) and body events (jako_body.c, jako_motion.cc: shake, cable in, battery full) live in this folder; see docs/body-events.md in the Jakomotchi repository.

The flight radar: the widget (jako_radar.c), the parser of the update's data and the seq gate (jako_radar_data.c, no device header), and Inter SemiBold at 20 and 22 px (font_inter_600_*_4.c, made by firmware/face-preview/gen_fonts.py, under the licence in font_inter_OFL.txt). firmware/face-preview/render.sh renders and checks them on the Mac; see docs/radar.md in the Jakomotchi repository.

The dial of today (jako_dial.c, its parser jako_dial_data.c), Jako's pictures (the scene widget jako_scene.c, its parser jako_scene_data.c) and the screen slot that decides what lies over the face (jako_screens.c, a pure reducer with no device header) live here too. The face's small mode is JAKO_FACE_SHOW_SMALL in jako_face.c. firmware/face-preview/render.sh renders and checks all of them on the Mac; see docs/screens.md in the Jakomotchi repository.
