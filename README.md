# mpvif

This is a patch and C plugin for mpv which makes mpv control a remote (typically nested or headless) Wayland compositor using virtual-keyboard and virtual-pointer protocols.

The intended use case is for upscaling 2D games using popular upscaling techniques already available for mpv. The game is run under a compatible compositor which is screen captured to a pipe or v4l2loopback device read by mpv which can perform processing on the video such as upscaling. Keyboard and pointer input on the mpv window is ignored by mpv and instead forwarded to a virtual keyboard and pointer on the compositor, essentially turning mpv into a remote desktop client.

Note that some of your shaders may not activate with RGB image formats, so you need to find RGB variants of them. Otherwise, you can convert the video to YUV444 and be careful not to mess up the colours in the process. Using `--vf=format=fmt=yuv444p10:gamma=bt.1886:convert=yes` seems to look ok. Probably the most popular and interesting shader which works without needing image format conversion is currently https://funnyplanter.github.io.

## Features

For more details, see the [mpvif branch on my repository](https://github.com/layercak3/mpv/tree/mpvif) and the files in this repository.

Functionality which requires access to objects on the VO connection (e.g. the shell surface) need to be implemented in the VO. Also, keyboard and pointer buttons are forwarded in the VO, to avoid unnecessary lossy translation from mpv keys. Other things can be implemented in a C plugin under mpvif-plugin/ so that there is less VO code to maintain/rebase.

### Keyboard input

Events are forwarded in the vo.

### Pointer input

This is a bit more complicated. Button and axis events are forwarded in the vo. However, motion isn't forwarded and does reach the mpv core. Motion is forwarded in the C plugin instead. This is because window positions may not match video positions. 100,100 on the window may not refer to 100,100 on the source video (remote Wayland output) because of black bars, panning, and scaling. The C plugin observes the `mouse-pos` property and calculates the correct motion request to send to the remote compositor using the `osd-dimensions` and `video-params` properties.

### Clipboard synchronization

This is trivial but not implemented yet.

### Pointer locks, confinement, relative motion, auto cursor placement (game warping the cursor)

Not yet investigated. Most of these will probably require private protocol/IPC to get the needed information from the remote compositor.

### Input method relay

Instead of needing to run another IME instance in the guest, mpv could become an input-method-v2 client on the remote compositor and forward the text-input-v3 events that it receives through it. Not yet investigated, but to get the cursor rectangle region this would require private protocol/IPC.

### Title synchronization

The C plugin manages the media title (it sets the `--force-media-title` option during runtime). It sets the media title to "Remote desktop [${wayland-remote-display-name} ${wayland-remote-output-name} ${wayland-remote-seat-name}]". If the remote compositor supports the wlr-foreign-toplevel-management protocol, "Remote desktop" will be replaced with the app ID and title of the currently fullscreened toplevel whenever appropriate.

### Cursor image synchronization

Instead of hiding the host cursor and letting the guest cursor be visible and scaled, it would be nicer for the image to be transferred so that mpv can set it as its cursor image unscaled, and have the guest cursor be hidden. This would require private protocol/IPC.

## Usage

The game should be run in a compositor which supports the virtual-keyboard and virtual-pointer protocols and has a nested or headless backend. Then, use screen capture software to record the output of the remote compositor that the game is placed on to rawvideo over a pipe or v4l2loopback device which mpv can playback.

Build [this](https://github.com/layercak3/mpv/tree/mpvif) mpv branch and set `--wayland-remote-display-name` to the path of the Wayland socket belonging to the remote compositor. Also set `--wayland-remote-output-name` to the name of the output being captured by whatever capture program you're using and `--wayland-remote-seat-name` to the seat that you want the virtual keyboard and pointer to be added to.

Also compile and load the C plugin located in mpvif-plugin/.

You'll probably want to use a dedicated minimal mpv config separate from your normal one. Also use the low-latency profile.

mpv can still be controlled using the input IPC server or the terminal. A key binding to temporarily escape input forwarding and show the cursor may be implemented in the future.

## Alternative implementation

Using mpv window embedding (`--wid`) over a dedicated input surface client is not possible on Wayland. Using the libmpv render API has limitations compared to standalone mpv VOs so I'm hesitant to use that. Modifying the wayland VO code is also the simplest implementation.

## License

The [patches to mpv](https://github.com/layercak3/mpv/tree/mpvif) are licensed under LGPL-2.1-or-later. Other code in this repository is licensed under GPL-3.0-or-later, see COPYING.

## Similar projects

* https://github.com/arzeth/cdmpv (`mpv --wid` over a VNC client)
* https://github.com/Blinue/Magpie (Window upscaler for Microsoft Windows with popular 2D scaling methods ported)
* https://github.com/ValveSoftware/gamescope (Nested compositor, more efficient and less hacky but the amount of scaling methods is relatively limited)
