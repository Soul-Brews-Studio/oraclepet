/// Native MQTT client — plain TCP on port 1883 via dart:io socket.
/// Used on macOS / iOS / Android / Linux / Windows.
import 'package:mqtt_client/mqtt_client.dart';
import 'package:mqtt_client/mqtt_server_client.dart';

MqttClient createMqttClient({
  required String host,
  required int port,
  required String clientId,
}) {
  return MqttServerClient.withPort(host, clientId, port);
}
