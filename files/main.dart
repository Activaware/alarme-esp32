import 'dart:async';
import 'dart:convert';
import 'dart:io' show Platform;

import 'package:flutter/material.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:speech_to_text/speech_to_text.dart';
import 'package:url_launcher/url_launcher.dart';

import 'comandos_voz.dart';

// Nordic UART Service — mesmos UUIDs do firmware do controlador
const serviceUuid = '6e400001-b5a0-f393-e0a9-e50e24dcca9e';
const rxUuid = '6e400002-b5a0-f393-e0a9-e50e24dcca9e'; // celular -> ESP32
const txUuid = '6e400003-b5a0-f393-e0a9-e50e24dcca9e'; // ESP32 -> celular

void main() => runApp(const AlarmeApp());

class AlarmeApp extends StatelessWidget {
  const AlarmeApp({super.key});

  @override
  Widget build(BuildContext context) => MaterialApp(
        title: 'Alarme ESP32',
        theme: ThemeData(
          colorSchemeSeed: Colors.red,
          useMaterial3: true,
          brightness: Brightness.dark,
        ),
        home: const Painel(),
      );
}

class Painel extends StatefulWidget {
  const Painel({super.key});

  @override
  State<Painel> createState() => _PainelState();
}

class _PainelState extends State<Painel> {
  final _stt = SpeechToText();
  BluetoothDevice? _dispositivo;
  BluetoothCharacteristic? _rx;
  StreamSubscription<List<int>>? _subNotify;
  StreamSubscription<BluetoothConnectionState>? _subConexao;
  String _buffer = '';
  String _estado = 'DESCONECTADO';
  String _ultimaFala = '';
  double? _lat, _lng;
  String _velocidade = '-';
  String _satelites = '-';
  bool _ouvindo = false;
  bool _conectando = false;
  final List<String> _linhas = [];

  void _addLog(String s) {
    if (!mounted) return;
    setState(() {
      _linhas.insert(0, s);
      if (_linhas.length > 40) _linhas.removeLast();
    });
  }

  // ---------------- BLE ----------------
  Future<void> _conectar() async {
    if (_conectando) return;
    setState(() => _conectando = true);
    try {
      if (await FlutterBluePlus.adapterState.first != BluetoothAdapterState.on) {
        _addLog('Ligue o Bluetooth do celular');
        return;
      }
      _addLog('Procurando o alarme...');

      // onScanResults só emite resultados DESTA busca (scanResults pode reemitir antigos)
      final achado = Completer<BluetoothDevice?>();
      final subScan = FlutterBluePlus.onScanResults.listen((lista) {
        if (lista.isNotEmpty && !achado.isCompleted) achado.complete(lista.first.device);
      });
      await FlutterBluePlus.startScan(
        withServices: [Guid(serviceUuid)],
        timeout: const Duration(seconds: 8),
      );
      final dispositivo =
          await achado.future.timeout(const Duration(seconds: 9), onTimeout: () => null);
      await subScan.cancel();
      await FlutterBluePlus.stopScan();
      if (dispositivo == null) {
        _addLog('Alarme não encontrado');
        return;
      }

      // flutter_blue_plus >= 2.0 exige `license: License.free` (uso não comercial).
      await dispositivo.connect();
      _dispositivo = dispositivo;
      _subConexao?.cancel();
      _subConexao = dispositivo.connectionState.listen((s) {
        if (s == BluetoothConnectionState.disconnected) _aoDesconectar();
      });

      // Android: inicia o pareamento (o celular pede o PIN de 6 dígitos do firmware).
      // No iOS o pedido aparece sozinho ao acessar a característica protegida.
      if (Platform.isAndroid) {
        try {
          await dispositivo.createBond();
        } catch (_) {}
      }

      final servicos = await dispositivo.discoverServices();
      for (final s in servicos) {
        if (s.uuid != Guid(serviceUuid)) continue;
        for (final c in s.characteristics) {
          if (c.uuid == Guid(rxUuid)) _rx = c;
          if (c.uuid == Guid(txUuid)) {
            await c.setNotifyValue(true);
            _subNotify?.cancel();
            _subNotify = c.onValueReceived.listen(_aoReceber);
          }
        }
      }
      if (_rx == null) {
        _addLog('Serviço do alarme não encontrado no dispositivo');
        await dispositivo.disconnect();
        return;
      }
      if (mounted) setState(() => _estado = 'CONECTADO');
      _addLog('Conectado');
      await _enviar('STATUS');
      await _enviar('RASTREAR:ON');
    } catch (e) {
      _addLog('Erro ao conectar: $e');
    } finally {
      if (mounted) setState(() => _conectando = false);
    }
  }

  void _aoDesconectar() {
    _subNotify?.cancel();
    _subNotify = null;
    _rx = null;
    _buffer = '';
    if (mounted) setState(() => _estado = 'DESCONECTADO');
    _addLog('Desconectado do alarme');
  }

  // O ESP32 envia em pedaços de 20 bytes; remonta pelo '\n'
  void _aoReceber(List<int> bytes) {
    _buffer += utf8.decode(bytes, allowMalformed: true);
    while (_buffer.contains('\n')) {
      final i = _buffer.indexOf('\n');
      final linha = _buffer.substring(0, i).trim();
      _buffer = _buffer.substring(i + 1);
      if (linha.isNotEmpty) _tratarLinha(linha);
    }
  }

  void _tratarLinha(String l) {
    if (!mounted) return;
    if (l.startsWith('ESTADO:')) {
      setState(() => _estado = l.substring(7));
    } else if (l.startsWith('GPS:')) {
      final partes = l.substring(4).split(',');
      if (partes.length >= 4) {
        setState(() {
          _lat = double.tryParse(partes[0]);
          _lng = double.tryParse(partes[1]);
          _velocidade = '${partes[2]} km/h';
          _satelites = partes[3];
        });
      }
    } else if (l.startsWith('ALERTA:')) {
      _addLog('🚨 $l');
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(
          backgroundColor: Colors.red,
          content: Text('ALARME DISPARADO — ${l.substring(7)}'),
        ),
      );
    } else {
      _addLog(l);
    }
  }

  Future<void> _enviar(String cmd) async {
    if (_rx == null) {
      _addLog('Não conectado');
      return;
    }
    try {
      await _rx!.write(utf8.encode(cmd));
      _addLog('→ $cmd');
    } catch (e) {
      _addLog('Falha ao enviar $cmd: $e');
    }
  }

  // ---------------- Voz ----------------
  Future<bool> _confirmar(String cmd, String fala) async {
    if (!mounted) return false;
    final r = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: Text('Confirmar $cmd?'),
        content: Text('Você disse: "$fala"'),
        actions: [
          TextButton(onPressed: () => Navigator.pop(ctx, false), child: const Text('Cancelar')),
          FilledButton(onPressed: () => Navigator.pop(ctx, true), child: Text(cmd)),
        ],
      ),
    );
    return r ?? false;
  }

  Future<void> _comandoPorVoz(String fala) async {
    final cmd = mapearComando(fala);
    if (cmd == null) {
      _addLog('Não entendi: "$fala"');
      return;
    }
    // Desarmar/destravar por voz exigem confirmação na tela.
    if (comandosSensiveis.contains(cmd) && !await _confirmar(cmd, fala)) {
      _addLog('Cancelado: $cmd');
      return;
    }
    await _enviar(cmd);
  }

  Future<void> _alternarVoz() async {
    if (_ouvindo) {
      await _stt.stop();
      setState(() => _ouvindo = false);
      return;
    }
    final ok = await _stt.initialize(
      onError: (e) {
        _addLog('Voz: ${e.errorMsg}');
        if (mounted) setState(() => _ouvindo = false);
      },
      onStatus: (s) {
        if ((s == 'done' || s == 'notListening') && mounted) setState(() => _ouvindo = false);
      },
    );
    if (!ok) {
      _addLog('Reconhecimento de voz indisponível');
      return;
    }
    setState(() => _ouvindo = true);
    await _stt.listen(
      localeId: 'pt_BR',
      onResult: (r) {
        if (!r.finalResult) return;
        setState(() {
          _ouvindo = false;
          _ultimaFala = r.recognizedWords;
        });
        _comandoPorVoz(r.recognizedWords);
      },
    );
  }

  Future<void> _abrirMapa() async {
    if (_lat == null || _lng == null) return;
    final uri = Uri.parse('https://www.google.com/maps/search/?api=1&query=$_lat,$_lng');
    await launchUrl(uri, mode: LaunchMode.externalApplication);
  }

  @override
  void dispose() {
    _subNotify?.cancel();
    _subConexao?.cancel();
    _stt.cancel();
    _dispositivo?.disconnect();
    super.dispose();
  }

  // ---------------- UI ----------------
  Widget _botao(String rotulo, String cmd, {Color? cor}) => FilledButton(
        style: cor == null ? null : FilledButton.styleFrom(backgroundColor: cor),
        onPressed: () => _enviar(cmd),
        child: Text(rotulo),
      );

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Alarme ESP32')),
      floatingActionButton: FloatingActionButton.large(
        onPressed: _alternarVoz,
        child: Icon(_ouvindo ? Icons.stop : Icons.mic),
      ),
      body: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(_estado, style: Theme.of(context).textTheme.headlineMedium),
            if (_ultimaFala.isNotEmpty) Text('Você disse: "$_ultimaFala"'),
            const SizedBox(height: 12),
            Wrap(spacing: 8, runSpacing: 8, children: [
              FilledButton.tonal(
                onPressed: _conectando ? null : _conectar,
                child: Text(_conectando ? 'Conectando...' : 'Conectar'),
              ),
              _botao('Armar', 'ARMAR'),
              _botao('Desarmar', 'DESARMAR'),
              _botao('Travar', 'TRAVAR'),
              _botao('Destravar', 'DESTRAVAR'),
              _botao('Achar carro', 'BUSCAR'),
              _botao('Pânico', 'PANICO', cor: Colors.red),
            ]),
            const Divider(height: 32),
            Text(_lat == null
                ? 'Sem posição GPS ainda'
                : 'Lat ${_lat!.toStringAsFixed(6)}  Lng ${_lng!.toStringAsFixed(6)}\n'
                    'Velocidade: $_velocidade  |  Satélites: $_satelites'),
            TextButton.icon(
              onPressed: _lat == null ? null : _abrirMapa,
              icon: const Icon(Icons.map),
              label: const Text('Abrir no mapa'),
            ),
            const Divider(),
            Expanded(
              child: ListView(
                children: [for (final l in _linhas) Text(l, style: const TextStyle(fontSize: 12))],
              ),
            ),
          ],
        ),
      ),
    );
  }
}
