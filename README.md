# waymetric

The waymetric component is an application for measuring the impact of Wayland (Display Server) on graphics performance.  Since waymetric uses EGL interface, some platform specific code is required for establishing an EGL context. This application has the implementation for devices using DRM (Direct Rendering Manager) using GBM (Graphic Buffer Management) and RaspberryPI devices using userland.  To add support to additional devices classes start with template/platform.cpp and replace the TBD's with appropriate code.

The test measures the overhead incurred when rendering using Wayland as compared to rendering directly with EGL.  If rendering with Wayland introduces no overhead then the Waymetric score would be 1.0.  If rendering with Wayland does introduce some overhead the Waymetric score would be greater than 1.  Hence the lower the Waymetric score the better.  However, it is possible for a waymetric score to be less than 1.   Consider a device employing DRM page flipping.  New frames are flipped to the display by calls to drmModePageFlip which is performing the flip during the vertical interval, so at 16.67ms intervals.  For EGL direct this application's call to eglSwapBuffers will return after the next vertical interval is reached and so this delay is in series with the rendering time whereas with Wayland, the swap interval is in parallel with the rendering time.  So as the rendering time per frame increases (modeled by waymetric as a delay of increasing size inserted into the render loop) when the render time plus the interval until the next vertical exceeds 16.67 ms, the frame rate drops from 60 fps to 30 fps, whereas with Wayland the client render time is in parallel with the compositor's page flip delay so the frame rate only begins to drop when the render time itself exceeds 16.67 ms.  Averaged across the measurement set, this reduces the metric score below 1.0.

# Running

The test has the following command line syntax:

```
waymetric <options> [<report-file>]
where
options are one of:
--window-size <width>x<height> (eg --window-size 640x480)
--iterations <count>
--no-direct
--no-wayland
--no-wayland-render
-? : show usage
```

After the test runs (which could take about 3 minutes) the output report file will be in /tmp/waymetric-report.txt (or whereever indicated by the invocation arguments).


