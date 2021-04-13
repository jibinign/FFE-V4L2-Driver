# FFE-V4L2-Driver
V4L2 driver with Frame Feed Emulator


1. Build the project using make.

		$ make


2. Insert module

		$ sudo insmod ffe_v4l2.ko


3. dmeseg will give the node name

		$ dmesg

	eg:- ffe_v4l2 ffe_v4l2: p_probe: V4L2 device registered as video1


4. play test video using any tools

	a) FFPLAY
	
		$ ffplay /dev/video1
		
		$ ffplay -framerate 30 /dev/video1
		
		$ ffplay -video_size 1280x720 /dev/video1
	
	b) MPLAYER
		
		$ mplayer tv:// -tv driver=v4l2:device=/dev/video1:width=1280:height=720:fps=30:outfmt=yuy2
		
		$ mplayer tv:// -tv driver=v4l2:device=/dev/video1:width=1280:height=720:fps=30:outfmt=mjpg
	
	c) GStreamer Pipeline
	
		$ gst-launch-1.0 v4l2src device=/dev/video1 ! videoconvert ! video/x-raw, width=1280, height=720 ! autovideosink
