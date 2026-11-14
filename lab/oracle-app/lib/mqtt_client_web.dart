/// Web MQTT client — WebSocket transport (Mosquitto default WS port 8083).
/// Browsers can't open raw TCP sockets so we tunnel MQTT over WS.
///
/// If mqtt.example.com doesn't expose a WebSocket listener, this connection
/// will fail and the UI will fall back to cached defaults (offline mode).
import 'package:mqtt_client/mqtt_client.dart';
import 'package:mqtt_client/mqtt_browser_client.dart';

MqttClient createMqttClient({
  required String host,
  required int port,
  required String clientId,
}) {
  // Mosquitto's default WebSocket port. If the broker uses a different port
  // for WS, edit this — port arg comes in as TCP port, which we don't use.
  final wsPort = 8083;
  final client = MqttBrowserClient.withPort('ws://$host', clientId, wsPort);
  client.websocketProtocols = ['mqtt'];
  return client;
}
