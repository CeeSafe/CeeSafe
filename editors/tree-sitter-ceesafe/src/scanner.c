// SPDX-License-Identifier: MIT
// The raw string literal of CeeSafe (spec/grammar.md §1.6): `"""` that ends its line, the lines that follow, and a
// closing line that holds only spaces or tabs and then `"""`. Tree-sitter's lexer takes the longest match, so it
// cannot stop at the first closing line; this scanner does.
#include "tree_sitter/parser.h"

enum TokenType { RAW_STRING_LITERAL };

void *tree_sitter_ceesafe_external_scanner_create(void) { return 0; }
void tree_sitter_ceesafe_external_scanner_destroy(void *payload) { (void)payload; }
unsigned tree_sitter_ceesafe_external_scanner_serialize(void *payload, char *buffer) {
  (void)payload;
  (void)buffer;
  return 0;
}
void tree_sitter_ceesafe_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
  (void)payload;
  (void)buffer;
  (void)length;
}

static void advance(TSLexer *lexer) { lexer->advance(lexer, false); }

static bool line_end(TSLexer *lexer) {
  if (lexer->lookahead == '\r') {
    advance(lexer);
  }
  if (lexer->lookahead != '\n') {
    return false;
  }
  advance(lexer);
  return true;
}

bool tree_sitter_ceesafe_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
  (void)payload;
  if (!valid_symbols[RAW_STRING_LITERAL]) {
    return false;
  }
  while (lexer->lookahead == ' ' || lexer->lookahead == '\t' || lexer->lookahead == '\n' ||
         lexer->lookahead == '\r' || lexer->lookahead == '\f') {
    lexer->advance(lexer, true);
  }
  for (int k = 0; k < 3; k++) {
    if (lexer->lookahead != '"') {
      return false;
    }
    advance(lexer);
  }
  while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
    advance(lexer);
  }
  if (!line_end(lexer)) {
    return false;
  }
  for (;;) {
    // At the start of a line: spaces or tabs, then either `"""` (the end) or the rest of a content line.
    while (lexer->lookahead == ' ' || lexer->lookahead == '\t') {
      advance(lexer);
    }
    int quotes = 0;
    while (quotes < 3 && lexer->lookahead == '"') {
      advance(lexer);
      quotes++;
    }
    if (quotes == 3) {
      lexer->result_symbol = RAW_STRING_LITERAL;
      return true;
    }
    while (lexer->lookahead != '\n' && lexer->lookahead != '\r') {
      if (lexer->eof(lexer)) {
        return false;
      }
      advance(lexer);
    }
    line_end(lexer);
  }
}
