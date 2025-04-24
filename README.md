# mpvif

This is a patch and C plugin for mpv which makes mpv control a headless Wayland compositor using virtual-keyboard and virtual-pointer protocols.

The intended use case is for upscaling 2D games using popular upscaling techniques already available for mpv. The game is run under a compatible compositor which is screen captured to a pipe or v4l2loopback device read by mpv which can perform processing on the video such as upscaling. Keyboard and pointer input on the mpv window is forwarded to a virtual keyboard and pointer on the compositor, essentially turning mpv into a remote desktop client.

Note that some of your shaders may not activate with RGB image formats, so you need to find RGB variants of them. Otherwise, you can convert the video to YUV444 and be careful not to mess up the colours in the process. Using `--vf=format=fmt=yuv444p10:gamma=bt.1886:convert=yes` seems to look ok. Probably the most popular and interesting shader which works without needing image format conversion is currently https://funnyplanter.github.io.

## Features

For more details, see the [mpvif branch on my repository](https://github.com/layercak3/mpv/tree/mpvif) and the files in this repository.

Functionality which requires access to objects on the VO connection (e.g. the shell surface) need to be implemented in the VO. Also, keyboard and pointer buttons are forwarded in the VO, to avoid unnecessary lossy translation from mpv keys. Other things can be implemented in a C plugin under mpvif-plugin/ so that there is less VO code to maintain/rebase.

### Keyboard input

Events are forwarded in the vo.

### Pointer input

This is a bit more complicated. Button and axis events are forwarded in the vo. However, motion is forwarded in the C plugin instead of the vo. This is because window positions may not match video positions. 100,100 on the window may not refer to 100,100 on the source video (remote Wayland output) because of black bars, panning, and scaling. The C plugin observes the `mouse-pos` property and calculates the correct motion request to send to the remote compositor using the `osd-dimensions` and `video-params` properties.

### Clipboard synchronization

This is trivial but not implemented yet.

### Pointer locks, confinement, relative motion, mouselook

When `--wayland-remote-force-grab-cursor` is enabled, the host pointer will be locked to the mpv window and relative motion requests will be emitted on the VO virtual pointer instead of absolute motion on the C plugin virtual pointer. This allows mouselook in 3D applications to work.

In order to automatically enable relative pointer/pointer constraints during the nested application's use of relative pointer/pointer constraints, and map the constraint regions, private protocol/IPC would be required.

### Auto cursor placement (game pointer warping)

When sway with [this patch](https://github.com/layercak3/sway/tree/ipc-event-cursor-warp) is the remote compositor, auto cursor placement is supported. The C plugin is notified by sway using i3 IPC when applications warp the pointer and it calculates the surface-local position using the `osd-dimensions` and `video-params` properties. A VO control is used to tell the VO to replicate the pointer warp on the host mpv window. This reuses the `mouse-pos` property, it is now read-write.

This mechanism isn't used or required when `--wayland-remote-force-grab-cursor` is enabled, as the host pointer would be locked in place and relative motion achieves the same effect.

To enable this, set `--wayland-remote-swaysock` to the path where sway's IPC is located (`$SWAYSOCK`).

In the future, this notification may be done with a private Wayland protocol instead of i3 IPC, but that involves writing more code across more projects.

#### layer shell?

Unfortunately, wlr-layer-shell cannot be used to get output-local pointer motion events unless it also takes pointer focus.

### Input method relay

Instead of needing to run another IME instance in the guest, mpv could become an input-method-v2 client on the remote compositor and forward the text-input-v3 events that it receives through it. Not yet investigated, but to get the cursor rectangle region this would require private protocol/IPC.

### Title synchronization

The C plugin manages the media title (it sets the `--force-media-title` option during runtime). It sets the media title to "Remote desktop [${wayland-remote-display-name} ${wayland-remote-output-name} ${wayland-remote-seat-name}]". If the remote compositor supports the wlr-foreign-toplevel-management protocol, "Remote desktop" will be replaced with the app ID and title of the currently fullscreened toplevel whenever appropriate.

### Cursor image synchronization

Instead of hiding the host cursor and letting the guest cursor be visible and scaled, it would be nicer for the image to be transferred so that mpv can set it as its cursor image unscaled, and have the guest cursor be hidden. This would require private protocol/IPC.

## Usage

The game should be run in a compositor which supports the virtual-keyboard and virtual-pointer protocols and has a nested or headless backend. Then, use screen capture software to record the output of the remote compositor that the game is placed on to rawvideo over a pipe or v4l2loopback device which mpv can playback.

Build [this](https://github.com/layercak3/mpv/tree/mpvif) mpv branch and the C plugin located in mpvif-plugin/. The patch is written in a way that it should be suitable to include in your normal mpv build, because if you don't set any of the `--wayland-remote-*` options, you can use mpv as normal.

The build process for the mpv branch is the exact same as upstream mpv. To build the C plugin, run `make` in the mpvif-plugin/ directory. You can then copy `mpvif-plugin.so` to your scripts directory or use `make install` which will install to ~/.config/mpv/scripts/ if running as non-root or to the system if running as root. The C plugin dependencies are a subset of what mpv requires.

Input forwarding is controlled by the `--wayland-remote-input-forwarding` option (can be changed at runtime), which is disabled by default. For it to work, the `--wayland-remote-display-name`, `--wayland-remote-output-name`, and `--wayland-remote-seat-name` options must be set before VO init. Please read the man page (DOCS/man/options.rst) for details.

When input forwarding is enabled, button and motion events will still reach the mpv core. You'll want to disable the osc and anything which reacts to mouse position, not load any keybindings (we will discuss how to enable keybindings at runtime), and prevent any of your scripts from loading key bindings. You should also hide the host cursor with `--cursor-autohide=always` and disable VO dragging with `--input-builtin-dragging=no`. More generally, you'll want to use a different, more minimal mpv config from your normal one (which should at the bare minimum contain `--profile=low-latency`).

To downgrade the "no key binding found" messages from warning to trace, you can use --input-downgrade-no-key-binding.

### Keybindings

As discussed, while input forwarding is enabled, mpv can still receive keybindings which would be disruptive. You should have a blank input.conf and use `--no-input-builtin-bindings`. However, it is possible to configure mpv such that you can switch between a "forwarding" mode and a "normal"/"command" mode.

Include only these lines in your input.conf:
```
Alt+PAUSE set cursor-autohide no; set wayland-remote-input-forwarding no; load-input-conf ~~home/input-normal.conf; enable-section mpvif
# Pseudo-key for window closing
CLOSE_WIN quit
```

Then, in input-normal.conf:
```
Alt+PAUSE {mpvif} set cursor-autohide always; set wayland-remote-input-forwarding yes; disable-section mpvif; load-input-conf ~~home/input.conf

# Include any key bindings you want, but with '{mpvif}' after the key name. See the mpv man page for details on the input.conf syntax.
q {mpvif} quit
f {mpvif} cycle fullscreen
Ctrl-g {mpvif} cycle-values glsl-shaders [...]
I {mpvif} script-binding stats/display-stats-toggle
[...]
```

You can now switch between forwarding and normal mode with `Alt+PAUSE`.

### Tips

This section will contain text which is not strictly related to the implementation of mpvif, but may be helpful.

#### Headless compositor

You can run sway headless with `WLR_RENDER_DRM_DEVICE=/dev/dri/XXX WLR_BACKENDS=headless sway -c /path/to/special/config` (see wlroots docs). You can also use other compositors as long as they support at least virtual-keyboard and virtual-pointer.

You should configure the HEADLESS-1 output in the config file or at runtime to the resolution that you want (typically the game native resolution at fullscreen). The refresh rate should be the maximum refresh rate you want to allow. If this is too high for your configuration and system, latency can suffer. You can get away with heavier shaders if you reduce the refresh rate of the compositor and game.

#### Recording to mpv

Example: `WAYLAND_DISPLAY=/path/to/headless/compositor wf-recorder -y -m rawvideo -c rawvideo -f pipe:1 -x bgra | mpv - --wayland-remote-display-name=/path/to/headless/compositor --wayland-remote-output-name=HEADLESS-1 --wayland-remote-seat-name=seat0 --demuxer=+rawvideo --demuxer-rawvideo-mp-format=bgra --demuxer-rawvideo-w=1280 --demuxer-rawvideo-h=720 --untimed`

For this example, you need to be using the master branch of wf-recorder, otherwise there is a bug which results in the image in mpv being shifted. Alternatively, you could redirect the pipes differently (`-f pipe:99 99>&1 >&2`), use `-m nut` instead of the rawvideo muxer, or use a v4l2loopback device instead of a pipe. Using v4l2 instead of a pipe is less performant.

If you need the video in YUV444 for certain shaders, you can add `--vf=format=fmt=yuv444p10:gamma=bt.1886:convert=yes`, which appears identical.

If you are trying other codecs and formats, you may or may not need `--untimed`, and you may have problems like the mpv window not appearing or mpv hanging while quitting due to waiting for frames from wf-recorder when the game/compositor is not drawing. You can add `-D -r $(fps)` to wf-recorder to stream at a constant refresh rate. This should not be needed with the rawvideo pipe example provided above.

#### Problems with seat keyboard/pointer hotplugging

With a headless compositor, there will be no keyboards or pointers except for mpvif's virtual keyboard and pointer(s). When mpv is not running, there will be no input devices (perhaps unless you are running an IME which uses virtual-keyboard in the headless display). Applications running will lose the keyboard and pointer capability on their seat until mpvif comes back. Some applications might have problems with this kind of hotplugging:

* Wine's Wayland driver may crash. This will be fixed in upstream wine soon.

As a workaround, you could write a daemon which creates an unused virtual keyboard/pointer and keep the connection alive, so that the applications never lose the keyboard/pointer capability.

## Alternative implementation

Using mpv window embedding (`--wid`) over a dedicated input surface client is not possible on Wayland. Using the libmpv render API has limitations compared to standalone mpv VOs so I'm hesitant to use that. Modifying the wayland VO code is also the simplest implementation.

## License

The [patches to mpv](https://github.com/layercak3/mpv/tree/mpvif) are licensed under LGPL-2.1-or-later. The C plugin in mpvif-plugin/ as a whole is licensed under GPL-3.0-or-later, see COPYING.

## Similar projects

* https://github.com/arzeth/cdmpv (`mpv --wid` over a VNC client)
* https://github.com/Blinue/Magpie (Window upscaler for Microsoft Windows with popular 2D scaling methods ported)
* https://github.com/ValveSoftware/gamescope (Nested compositor, more efficient and less hacky but the amount of scaling methods is relatively limited)
