# ffmpeg_playlist_live
Take a directory of mismatched videos and create a single stream with fixed audo/video format.

To build 

1. make sure you have ffmpeg installed and built
2. run make
3. "./playlister" will give you the options available


Examples

# Stream the video files in ./myvideos/ to a multicast udp address
./playlister -re -loop -random -width 640 -height 360 ./myvideos/ "udp://225.170.249.224:1234?pkt_size=1316&buffer_size=1048576

# Make a single video file with all the videos in ./myvideos/
# As this is a mkv chapter marks will be added
./playlister -random -width 640 -height 360 ./myvideos/ viewable.mkv

# Overlay 
Add an overlay at (0,0), this needs to be a transparent image and can be changed on the fly and the system will reload the new image when changed.

Enjoy

Joolz

