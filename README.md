# FFE-V4L2-Driver
V4L2 driver with Frame Feed Emulator


1. Build the project using make.

	$ make


2. Insert module

	$ sudo insmod ffe_v4l2.ko


3. dmeseg will give the node name

	$ dmesg

	eg:- ffe_v4l2 ffe_v4l2: p_probe: V4L2 device registered as video1


4. play test video using ffplay

	$ ffplay /dev/video1


5. Frame rate, video size can be changed

	$ ffplay -framerate 25 /dev/video1

	$ ffplay -video_size 1280x720 /dev/video1
