// Node does not populate Emscripten's guest ENV from process.env. Forward the
// rootless switch so the same Toolbox tests can run with or without the bridge.
Module.preRun = [function () {
    if (process.env.YORE_ROOTLESS_WINDOWS)
        ENV.YORE_ROOTLESS_WINDOWS = process.env.YORE_ROOTLESS_WINDOWS;
}];
