var ws_server;
var ws_port;

var rtc_configuration = {iceServers: [{urls: "stun:stun.l.google.com:19302"}]};
var peer_connection = new RTCPeerConnection(rtc_configuration);
var data_channel;
var ws_conn;
const remoteStream = new MediaStream();

function onIncomingICE(ice) {
    var candidate = new RTCIceCandidate(ice);
    peer_connection.addIceCandidate(candidate);
    // console.log("ice candidtae added")
}
function createAnswer() {
    console.log("Creating answer");
    peer_connection.createAnswer()
        .then(answer => {
            console.log("Setting local description");
            return peer_connection.setLocalDescription(answer);
        })
        .then(() => {
            console.log("Sending answer");
            ws_conn.send(JSON.stringify(peer_connection.localDescription));
        })}

// Call createAnswer when an offer is received
function onServerMessage(event) {
    // console.log("Received " + event.data);
    var msg = JSON.parse(event.data);
    if (msg.sdp != null) {
        console.log("Received SDP offer");
        peer_connection.setRemoteDescription(new RTCSessionDescription(msg))
            .then(() => {
                console.log("Remote description set successfully");
                createAnswer();
            })
    
    } else if (msg.ice != null) {
                onIncomingICE(msg.ice);
    }
}

function websocketServerConnect() {
    ws_port = ws_port || '8443';
    ws_server = ws_server || "192.168.68.103";
    var ws_url = 'ws://' + ws_server + ':' + ws_port
    console.log("Connecting to server " + ws_url);
    ws_conn = new WebSocket(ws_url);

    createCall();
    ws_conn.addEventListener('message', onServerMessage);
}

function createCall() {
    data_channel = peer_connection.createDataChannel('label', null);

       
    peer_connection.ontrack = (event) => {
        const track = event.track;
        console.log("-------------track kind = ", track.kind )
        if (track.kind === 'video') {
        remoteStream.addTrack(track);
        }
    };

    peer_connection.addEventListener("track", ({ streams: [stream] }) => {
        const video = document.getElementById("remote-video");
        video.srcObject = stream;
        video.onloadedmetadata = () => {
        video.play();
        };
    });
  

}
