import 'package:alarme_app/comandos_voz.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  void espera(String fala, String? cmd) =>
      test('"$fala" -> $cmd', () => expect(mapearComando(fala), cmd));

  group('o bug original ("alarme" contém "arm")', () {
    espera('status do alarme', 'STATUS');
    espera('qual a situação do alarme', 'STATUS');
    espera('como está o alarme', 'STATUS');
    espera('o alarme está armado?', null);
    espera('o alarme está ativo', null);
  });

  group('comandos básicos', () {
    espera('armar o alarme', 'ARMAR');
    espera('ativar o alarme', 'ARMAR');
    espera('desarmar', 'DESARMAR');
    espera('desativa o alarme', 'DESARMAR');
    espera('desligar o alarme', 'DESARMAR');
    espera('travar as portas', 'TRAVAR');
    espera('fecha o carro', 'TRAVAR');
    espera('destravar as portas', 'DESTRAVAR');
    espera('abrir o carro', 'DESTRAVAR');
    espera('pânico', 'PANICO');
    espera('socorro', 'PANICO');
    espera('onde está meu carro?', 'LOCALIZAR');
    espera('achar o carro', 'BUSCAR');
    espera('status', 'STATUS');
  });

  group('segurança', () {
    espera('não desarme o alarme', null);
    espera('nunca destrave', null);
    espera('desarmar e travar', null);      // ambíguo com comando sensível
    espera('abrir e armar', null);
    espera('armar e travar as portas', 'ARMAR');
    espera('', null);
    espera('bom dia', null);
  });
}
