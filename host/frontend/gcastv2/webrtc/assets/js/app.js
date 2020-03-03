'use strict';

const receiveButton = document.getElementById('receiveButton');
receiveButton.addEventListener('click', onReceive);
const keyboardCaptureButton = document.getElementById('keyboardCaptureBtn');
keyboardCaptureButton.addEventListener('click', onKeyboardCaptureClick);

const deviceScreen = document.getElementById('deviceScreen');

deviceScreen.addEventListener("click", onInitialClick);

function onInitialClick(e) {
    // This stupid thing makes sure that we disable controls after the first click...
    // Why not just disable controls altogether you ask? Because then audio won't play
    // because these days user-interaction is required to enable audio playback...
    console.log("onInitialClick");

    deviceScreen.controls = false;
    deviceScreen.removeEventListener("click", onInitialClick);
}

let videoStream;

let mouseIsDown = false;

let deviceConnection;

function onKeyboardCaptureClick(e) {
    const selectedClass = 'selected';
    if (keyboardCaptureButton.classList.contains(selectedClass)) {
        stopKeyboardTracking();
        keyboardCaptureButton.classList.remove(selectedClass);
    } else {
        startKeyboardTracking();
        keyboardCaptureButton.classList.add(selectedClass);
    }
}

async function onReceive() {
    console.log('onReceive');
    receiveButton.disabled = true;

    // init_logcat();

    let options = {
        // temporarily disable audio to free ports in the server since it's only
        // producing silence anyways.
        disable_audio: true,
        wsProtocol: (location.protocol == "http:") ? "ws:" : "wss:",
        wsPath: location.host + "/control",
    };
    let urlParams = new URLSearchParams(location.search);
    for (const [key, value] of urlParams) {
        options[key] = JSON.parse(value);
    }

    import('./cf_webrtc.js')
        .then(webrtcModule => webrtcModule.Connect('device_id', options))
        .then(devConn => {
            deviceConnection = devConn;
            videoStream = devConn.getVideoStream(0);
            deviceScreen.srcObject = videoStream;
            startMouseTracking(); // TODO stopMouseTracking() when disconnected
        });
}

function startMouseTracking() {
    if (window.PointerEvent) {
        deviceScreen.addEventListener("pointerdown", onStartDrag);
        deviceScreen.addEventListener("pointermove", onContinueDrag);
        deviceScreen.addEventListener("pointerup", onEndDrag);
    } else if (window.TouchEvent) {
        deviceScreen.addEventListener("touchstart", onStartDrag);
        deviceScreen.addEventListener("touchmove", onContinueDrag);
        deviceScreen.addEventListener("touchend", onEndDrag);
    } else if (window.MouseEvent) {
        deviceScreen.addEventListener("mousedown", onStartDrag);
        deviceScreen.addEventListener("mousemove", onContinueDrag);
        deviceScreen.addEventListener("mouseup", onEndDrag);
    }
}

function stopMouseTracking() {
    if (window.PointerEvent) {
        deviceScreen.removeEventListener("pointerdown", onStartDrag);
        deviceScreen.removeEventListener("pointermove", onContinueDrag);
        deviceScreen.removeEventListener("pointerup", onEndDrag);
    } else if (window.TouchEvent) {
        deviceScreen.removeEventListener("touchstart", onStartDrag);
        deviceScreen.removeEventListener("touchmove", onContinueDrag);
        deviceScreen.removeEventListener("touchend", onEndDrag);
    } else if (window.MouseEvent) {
        deviceScreen.removeEventListener("mousedown", onStartDrag);
        deviceScreen.removeEventListener("mousemove", onContinueDrag);
        deviceScreen.removeEventListener("mouseup", onEndDrag);
    }
}

function startKeyboardTracking() {
    document.addEventListener('keydown', onKeyEvent);
    document.addEventListener('keyup', onKeyEvent);
}

function stopKeyboardTracking() {
    document.removeEventListener('keydown', onKeyEvent);
    document.removeEventListener('keyup', onKeyEvent);
}

function onStartDrag(e) {
    e.preventDefault();

    // console.log("mousedown at " + e.pageX + " / " + e.pageY);
    mouseIsDown = true;

    sendMouseUpdate(true, e);
}

function onEndDrag(e) {
    e.preventDefault();

    // console.log("mouseup at " + e.pageX + " / " + e.pageY);
    mouseIsDown = false;

    sendMouseUpdate(false, e);
}

function onContinueDrag(e) {
    e.preventDefault();

    // console.log("mousemove at " + e.pageX + " / " + e.pageY + ", down=" + mouseIsDown);
    if (mouseIsDown) {
        sendMouseUpdate(true, e);
    }
}

function sendMouseUpdate(down, e) {
    console.assert(deviceConnection, 'Can\'t send mouse update without device');
    var x = e.offsetX;
    var y = e.offsetY;

    // TODO get the device's screen resolution from the device config, not the video stream
    const videoWidth = deviceScreen.videoWidth;
    const videoHeight = deviceScreen.videoHeight;
    const elementWidth = deviceScreen.offsetWidth;
    const elementHeight = deviceScreen.offsetHeight;

    // vh*ew > eh*vw? then scale h instead of w
    const scaleHeight = videoHeight * elementWidth > videoWidth * elementHeight;
    var elementScaling = 0, videoScaling = 0;
    if (scaleHeight) {
        elementScaling = elementHeight;
        videoScaling = videoHeight;
    } else {
        elementScaling = elementWidth;
        videoScaling = videoWidth;
    }

    // Substract the offset produced by the difference in aspect ratio if any.
    if (scaleHeight) {
        x -= (elementWidth - elementScaling * videoWidth / videoScaling) / 2;
    } else {
        y -= (elementHeight - elementScaling * videoHeight / videoScaling) / 2;
    }

    // Convert to coordinates relative to the video
    x = videoScaling * x / elementScaling;
    y = videoScaling * y / elementScaling;

    deviceConnection.sendMousePosition(Math.trunc(x), Math.trunc(y), down);
}

function onKeyEvent(e) {
    e.preventDefault();
    console.assert(deviceConnection, 'Can\'t send key event without device');
    deviceConnection.sendKeyEvent(e.code, e.type);
}