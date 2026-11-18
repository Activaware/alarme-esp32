/// Converte uma frase falada (pt-BR) em um comando do protocolo BLE.
///
/// Correção em relação ao `_mapear` original: ele usava `contains('arm')`,
/// que casa com "al-ARM-e" ("status do alarme" virava ARMAR) e com "armado"
/// ("o alarme está armado?" também armava). Aqui:
///  1. o texto é normalizado (minúsculas, sem acento, só letras/números);
///  2. cada comando tem uma lista de palavras INTEIRAS (\b...\b), sem prefixos;
///  3. negações ("não desarme") cancelam tudo;
///  4. se a frase casar com DESARMAR/DESTRAVAR e com outro comando, é
///     considerada ambígua e nada é enviado.
library;

/// Comandos que reduzem a segurança: o app pede confirmação antes de enviar por voz.
const comandosSensiveis = {'DESARMAR', 'DESTRAVAR'};

// Cada linha: comando -> padrão (texto já normalizado, ASCII).
const _padroes = <String, String>{
  'PANICO': r'\b(panico|socorro|emergencia)\b',
  'DESARMAR':
      r'\b(desarmar|desarme|desarma|desativar|desative|desativa|desligar o alarme|desligue o alarme)\b',
  'ARMAR': r'\b(armar|arme|arma|ativar|ative|ativa|ligar o alarme|ligue o alarme)\b',
  'DESTRAVAR':
      r'\b(destravar|destrave|destrava|destrancar|destranque|destranca|desbloquear|desbloqueie|abrir|abra|abre)\b',
  'TRAVAR': r'\b(travar|trave|trava|trancar|tranque|tranca|fechar|feche|fecha|bloquear|bloqueie)\b',
  'BUSCAR': r'\b(buscar|busque|busca|achar|ache|acha|encontrar|encontre|encontra)\b',
  'LOCALIZAR':
      r'\b(onde|localizar|localize|localiza|rastrear|rastreie|rastreia|localizacao|posicao)\b',
  'STATUS': r'\b(status|situacao|estado)\b|\bcomo esta\b',
};

final _negacao = RegExp(r'\b(nao|nunca|jamais|sem)\b');

final _regras = {for (final e in _padroes.entries) e.key: RegExp(e.value)};

/// Ordem de desempate quando a frase casa com mais de um comando "seguro".
const _prioridade = ['PANICO', 'BUSCAR', 'LOCALIZAR', 'ARMAR', 'TRAVAR', 'STATUS'];

const _acentos = 'áàâãäéèêëíìîïóòôõöúùûüç';
const _semAcentos = 'aaaaaeeeeiiiiooooouuuuc';

String normalizar(String texto) {
  final sb = StringBuffer();
  for (final r in texto.toLowerCase().runes) {
    final ch = String.fromCharCode(r);
    final i = _acentos.indexOf(ch);
    sb.write(i >= 0 ? _semAcentos[i] : ch);
  }
  return sb.toString().replaceAll(RegExp(r'[^a-z0-9]+'), ' ').trim();
}

/// Retorna o comando ou `null` se não entendeu / for ambíguo / houver negação.
String? mapearComando(String fala) {
  final t = normalizar(fala);
  if (t.isEmpty || _negacao.hasMatch(t)) return null;

  final achados = [
    for (final e in _regras.entries)
      if (e.value.hasMatch(t)) e.key,
  ];
  if (achados.isEmpty) return null;
  if (achados.length == 1) return achados.first;

  // Mais de um comando: nunca arrisque desarmar/destravar.
  if (achados.any(comandosSensiveis.contains)) return null;
  for (final c in _prioridade) {
    if (achados.contains(c)) return c;
  }
  return null;
}
