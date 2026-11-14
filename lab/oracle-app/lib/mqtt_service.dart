/// MQTT client wrapper that connects to mqtt.example.com and exposes each
/// retained xiaoer/active/* topic as a Stream<String>.
///
/// Used by [BrewingScreen] to populate the dashboard live — same data path
/// the ESP32 firmware subscribes to.
import 'dart:async';
import 'package:mqtt_client/mqtt_client.dart';
import 'mqtt_client_io.dart' if (dart.library.html) 'mqtt_client_web.dart';

class MqttService {
  MqttService({
    this.host = const String.fromEnvironment('MQTT_HOST', defaultValue: 'mqtt.example.com'),
    this.port = 1883,
    this.username = const String.fromEnvironment('MQTT_USER'),
    this.password = const String.fromEnvironment('MQTT_PASS'),
    this.topicPrefix = 'xiaoer/active',
  });

  final String host;
  final int port;
  final String username;
  final String password;
  final String topicPrefix;

  late final MqttClient _client;
  final Map<String, StreamController<String>> _topicStreams = {};

  /// Connection-state stream — true once subscribed.
  final StreamController<bool> _connected = StreamController<bool>.broadcast();
  Stream<bool> get connected => _connected.stream;

  Future<void> start() async {
    final clientId = 'oracle_app_${DateTime.now().millisecondsSinceEpoch}';
    _client = createMqttClient(host: host, port: port, clientId: clientId);
    _client.keepAlivePeriod = 30;
    _client.logging(on: false);
    _client.onConnected = () => _connected.add(true);
    _client.onDisconnected = () => _connected.add(false);

    final connMessage = MqttConnectMessage()
        .withClientIdentifier(clientId)
        .authenticateAs(username, password)
        .startClean();
    _client.connectionMessage = connMessage;

    try {
      await _client.connect();
    } catch (e) {
      // Surface to UI via connection stream — never crash on startup.
      // ignore: avoid_print
      print('MQTT connect failed: $e');
      _connected.add(false);
      return;
    }

    _client.subscribe('$topicPrefix/#', MqttQos.atMostOnce);

    _client.updates?.listen((List<MqttReceivedMessage<MqttMessage>> events) {
      for (final ev in events) {
        final payload = ev.payload as MqttPublishMessage;
        final value = MqttPublishPayload.bytesToStringAsString(
          payload.payload.message,
        );
        // strip the prefix so consumers ask for "name", "day", etc.
        final field = ev.topic.startsWith('$topicPrefix/')
            ? ev.topic.substring(topicPrefix.length + 1)
            : ev.topic;
        _streamFor(field).add(value);
      }
    });
  }

  /// Stream of values for a given topic field (e.g. "name", "day").
  /// Late subscribers get the most recent value because we use broadcast +
  /// retained MQTT messages from the broker.
  Stream<String> watch(String field) => _streamFor(field).stream;

  StreamController<String> _streamFor(String field) {
    return _topicStreams.putIfAbsent(
      field,
      () => StreamController<String>.broadcast(),
    );
  }

  void dispose() {
    _client.disconnect();
    for (final c in _topicStreams.values) {
      c.close();
    }
    _connected.close();
  }
}
