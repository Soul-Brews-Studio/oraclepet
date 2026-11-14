/// Brewing dashboard — Flutter rebuild of the ESP32 LVGL v2 layout.
///
/// 10 cards mapping 1:1 to lab/jc3248w535/jc3248w535.yaml + sim/main_v2.c:
///   1. Header — Xiaoer Oracle + FERMENTING pill
///   2. DAY counter + progress bar
///   3. BREWING — Neo Genesis / American IPA / ESP32 chip
///   4. VOLUME card
///   5. GRAIN card
///   6-9. OG / FG / ABV / IBU mini cards
///   10. TENDING THE FLAME bar
import 'package:flutter/material.dart';
import 'mqtt_service.dart';

class BrewingScreen extends StatefulWidget {
  const BrewingScreen({super.key, required this.mqtt});

  final MqttService mqtt;

  @override
  State<BrewingScreen> createState() => _BrewingScreenState();
}

class _BrewingScreenState extends State<BrewingScreen> {
  // Placeholders until MQTT lands. Recipe-target fields (name, style,
  // batch size, grain, ABV, IBU) are baked in — they're the planned recipe
  // and don't change at runtime. Live fields (day, total_days, status) use
  // "?" so a broken data path is obvious instead of silent.
  final Map<String, String> _state = {
    'name': 'Neo Genesis',
    'style': 'American IPA',
    'status': '...',
    'day': '?',
    'total_days': '?',
    'batch_size_l': '530',
    'grain_kg': '139',
    'abv_target': '6.6',
    'ibu': '66',
  };
  bool _mqttUp = false;

  @override
  void initState() {
    super.initState();
    widget.mqtt.connected.listen((up) {
      if (mounted) setState(() => _mqttUp = up);
    });
    for (final field in _state.keys) {
      widget.mqtt.watch(field).listen((value) {
        if (!mounted || value.isEmpty) return;
        setState(() => _state[field] = value);
      });
    }
  }

  static const _bgCard = Color(0xFF0A1419);
  static const _border = Color(0xFF1F3540);
  static const _accent = Color(0xFF00FF7F);
  static const _blue = Color(0xFF4FB3FF);
  static const _orange = Color(0xFFFFD27F);
  static const _muted = Color(0xFF888888);
  static const _fg = Color(0xFFFFFFFF);
  static const _fgDim = Color(0xFFCCCCCC);

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: Colors.black,
      body: SafeArea(
        child: Center(
          child: ConstrainedBox(
            constraints: const BoxConstraints(maxWidth: 360),
            child: SingleChildScrollView(
              padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 8),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.stretch,
                children: [
                  _header(),
                  const SizedBox(height: 8),
                  _dayCounter(),
                  const SizedBox(height: 8),
                  _brewing(),
                  const SizedBox(height: 8),
                  Row(children: [
                    Expanded(child: _volume()),
                    const SizedBox(width: 8),
                    Expanded(child: _grain()),
                  ]),
                  const SizedBox(height: 8),
                  Row(children: [
                    Expanded(child: _statBox('OG', '?.???', muted: true)),
                    const SizedBox(width: 6),
                    Expanded(child: _statBox('FG', '?.???', muted: true)),
                    const SizedBox(width: 6),
                    Expanded(child: _statBox('ABV', '${_state['abv_target']}%')),
                    const SizedBox(width: 6),
                    Expanded(child: _statBox('IBU', _state['ibu']!)),
                  ]),
                  const SizedBox(height: 8),
                  _tendingTheFlame(),
                  const SizedBox(height: 8),
                  _connectionPill(),
                ],
              ),
            ),
          ),
        ),
      ),
    );
  }

  Widget _card({required Widget child, Color borderColor = _border, double pad = 14}) {
    return Container(
      padding: EdgeInsets.all(pad),
      decoration: BoxDecoration(
        color: _bgCard,
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: borderColor, width: 1),
      ),
      child: child,
    );
  }

  Widget _header() => _card(
        child: Row(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  const Text('Xiaoer Oracle',
                      style: TextStyle(color: _fg, fontSize: 22, fontWeight: FontWeight.w400)),
                  const SizedBox(height: 4),
                  const Text('first body with a screen',
                      style: TextStyle(color: _muted, fontSize: 13)),
                ],
              ),
            ),
            _pill(_state['status']!),
          ],
        ),
      );

  Widget _pill(String text) => Container(
        padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 4),
        decoration: BoxDecoration(
          color: _bgCard,
          borderRadius: BorderRadius.circular(13),
          border: Border.all(color: _accent, width: 1),
        ),
        child: Text(text,
            style: const TextStyle(color: _accent, fontSize: 12, fontWeight: FontWeight.w500, letterSpacing: 0.5)),
      );

  Widget _dayCounter() {
    final day = int.tryParse(_state['day']!) ?? 0;
    final total = int.tryParse(_state['total_days']!) ?? 1;
    return _card(
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            crossAxisAlignment: CrossAxisAlignment.baseline,
            textBaseline: TextBaseline.alphabetic,
            children: [
              const Text('DAY', style: TextStyle(color: _muted, fontSize: 13, letterSpacing: 1)),
              const SizedBox(width: 12),
              Text('${_state['day']}',
                  style: const TextStyle(color: _accent, fontSize: 28, fontWeight: FontWeight.w500)),
              const SizedBox(width: 10),
              Text('/ ${_state['total_days']}',
                  style: const TextStyle(color: _muted, fontSize: 22)),
            ],
          ),
          const SizedBox(height: 10),
          ClipRRect(
            borderRadius: BorderRadius.circular(3),
            child: LinearProgressIndicator(
              value: (day / total).clamp(0.0, 1.0),
              minHeight: 6,
              backgroundColor: _border,
              valueColor: const AlwaysStoppedAnimation(_accent),
            ),
          ),
        ],
      ),
    );
  }

  Widget _brewing() => _card(
        child: Row(
          children: [
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  const Text('BREWING',
                      style: TextStyle(color: _blue, fontSize: 12, letterSpacing: 1)),
                  const SizedBox(height: 4),
                  Text(_state['name']!,
                      style: const TextStyle(color: _fg, fontSize: 26, fontWeight: FontWeight.w500)),
                  const SizedBox(height: 4),
                  Text(_state['style']!,
                      style: const TextStyle(color: _blue, fontSize: 13)),
                ],
              ),
            ),
            Column(
              mainAxisAlignment: MainAxisAlignment.center,
              children: const [
                Icon(Icons.memory, color: _blue, size: 22),
                SizedBox(height: 4),
                Text('ESP32', style: TextStyle(color: _blue, fontSize: 11)),
              ],
            ),
          ],
        ),
      );

  Widget _bigStatCard(IconData icon, String label, String value) => _card(
        pad: 12,
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(children: [
              Icon(icon, color: _blue, size: 16),
              const SizedBox(width: 6),
              Text(label, style: const TextStyle(color: _blue, fontSize: 12, letterSpacing: 1)),
            ]),
            const SizedBox(height: 10),
            Text(value, style: const TextStyle(color: _fg, fontSize: 22, fontWeight: FontWeight.w500)),
          ],
        ),
      );

  Widget _volume() => _bigStatCard(Icons.water_drop, 'VOLUME', '${_state['batch_size_l']} L');
  Widget _grain() => _bigStatCard(Icons.grain, 'GRAIN', '${_state['grain_kg']} kg');

  Widget _statBox(String label, String value, {bool muted = false}) => _card(
        pad: 6,
        child: Column(
          children: [
            Text(label, style: const TextStyle(color: _blue, fontSize: 11, letterSpacing: 1)),
            const SizedBox(height: 6),
            Text(value, style: TextStyle(color: muted ? _muted : _accent, fontSize: 14, fontWeight: FontWeight.w500)),
          ],
        ),
      );

  Widget _tendingTheFlame() => _card(
        borderColor: _orange,
        pad: 12,
        child: Row(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            const Icon(Icons.local_fire_department, color: _orange, size: 20),
            const SizedBox(width: 10),
            // StreamBuilder rebuilds just the timestamp without redrawing the whole tree.
            StreamBuilder<DateTime>(
              stream: Stream.periodic(const Duration(seconds: 2), (i) => DateTime.now()),
              builder: (context, snap) {
                final s = (snap.data?.millisecondsSinceEpoch ?? 0) ~/ 2000 % 100000;
                return Text('TENDING THE FLAME [$s]',
                    style: const TextStyle(color: _orange, fontSize: 12, letterSpacing: 1.2));
              },
            ),
          ],
        ),
      );

  Widget _connectionPill() => Center(
        child: Text(
          _mqttUp ? '● live · mqtt.example.com' : '○ offline · using cached defaults',
          style: TextStyle(color: _mqttUp ? _accent : _muted, fontSize: 11),
        ),
      );
}
