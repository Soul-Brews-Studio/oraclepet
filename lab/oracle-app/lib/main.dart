import 'package:flutter/material.dart';
import 'brewing_screen.dart';
import 'mqtt_service.dart';

void main() {
  runApp(const OracleApp());
}

class OracleApp extends StatefulWidget {
  const OracleApp({super.key});

  @override
  State<OracleApp> createState() => _OracleAppState();
}

class _OracleAppState extends State<OracleApp> {
  late final MqttService _mqtt;

  @override
  void initState() {
    super.initState();
    _mqtt = MqttService();
    _mqtt.start();
  }

  @override
  void dispose() {
    _mqtt.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'oracle-face',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        useMaterial3: true,
        brightness: Brightness.dark,
        scaffoldBackgroundColor: Colors.black,
      ),
      home: BrewingScreen(mqtt: _mqtt),
    );
  }
}
