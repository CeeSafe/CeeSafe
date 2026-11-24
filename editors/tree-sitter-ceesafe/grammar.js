// SPDX-License-Identifier: MIT
// A Tree-sitter grammar for CeeSafe, for editors that highlight with Tree-sitter (editors/zed). It follows
// spec/grammar.md and type-system.md §15.2 (patterns), and is permissive where an editor should be: what the compiler
// refuses with a message (a missing `(` after `if`, a tuple) may still parse here. The compiler's parser is the
// authority; tools/tree_sitter_check.py parses the compiler's, the libraries' and the corpus's sources with this one.

const PREC = {
  closure: -1,
  fallback: 1, // ??
  or: 2,
  and: 3,
  compare: 4, // == != < <= > >= is
  bitor: 5,
  bitxor: 6,
  bitand: 7,
  shift: 8,
  additive: 9,
  multiplicative: 10,
  cast: 11,
  unary: 12,
  postfix: 13,
  primary: 14,
};

const commaSep = (rule) => optional(commaSep1(rule));
const commaSep1 = (rule) => seq(rule, repeat(seq(",", rule)));
const sep1 = (separator, rule) => seq(rule, repeat(seq(separator, rule)));

module.exports = grammar({
  name: "ceesafe",

  word: ($) => $.identifier,

  externals: ($) => [$.raw_string_literal],

  extras: ($) => [/\s/, $.line_comment, $.block_comment],

  supertypes: ($) => [$._expression, $._type, $._pattern, $._statement],

  conflicts: ($) => [
    [$.scoped_type_identifier, $._expression, $.scoped_type_literal_name],
    [$.scoped_type_identifier, $._expression],
    [$.block, $.arm_block],
    [$.field_expression],
    [$._type, $.generic_type],
    [$.generic_type, $.generic_name],
    [$.generic_type, $._expression, $.generic_identifier],
    [$.generic_type, $._expression, $.generic_name],
    [$.constant_pattern, $.struct_pattern],
    [$.binding_pattern, $.struct_pattern],
    [$.or_pattern],
    [$.qualified_pattern_name, $.range_pattern],
    [$._pattern, $.range_pattern],
    [$.binding_pattern, $.range_pattern],
    [$.qualified_pattern_name],
    [$.closure_expression],
    [$._type, $.sources, $._expression, $.closure_parameter],
    [$.sources, $.closure_parameter],
    [$.sources, $.this],
    [$.binary_expression],
    [$.constant_pattern, $.variant_pattern],
    [$.binding_pattern, $.variant_pattern],
    [$._type, $._expression, $.closure_parameter],
    [$._type, $._expression],
    [$._type, $.sources],
    [$._expression, $.closure_parameter],
    [$._expression, $.struct_literal],
    [$._type, $.scoped_type_identifier],
    [$.let_declaration, $.binding_pattern],
    [$.qualified_pattern_name, $.binding_pattern],
    [$.range_pattern, $.constant_pattern],
  ],

  rules: {
    source_file: ($) => repeat(choice($.namespace_declaration, $.import_declaration, $._item)),

    namespace_declaration: ($) => seq("namespace", sep1(".", $.identifier), ";"),
    import_declaration: ($) => seq("import", sep1(".", $.identifier), ";"),

    // ---- Items ----

    _item: ($) =>
      choice(
        $.function_item,
        $.const_item,
        $.static_item,
        $.struct_item,
        $.enum_item,
        $.interface_item,
        $.impl_item,
        $.extern_block,
      ),

    attribute: ($) =>
      seq(
        "@",
        field("name", $.identifier),
        optional(seq("(", commaSep($._attribute_argument), optional(","), ")")),
      ),
    _attribute_argument: ($) =>
      choice($.identifier, $.string_literal, $.integer_literal, $.scoped_identifier, "this", "static"),

    visibility_modifier: () => choice("public", "internal", "private"),

    function_item: ($) =>
      seq(
        repeat($.attribute),
        optional($.visibility_modifier),
        optional("unsafe"),
        optional("async"),
        "fn",
        field("name", $.identifier),
        optional(field("type_parameters", $.type_parameters)),
        field("parameters", $.parameters),
        optional(seq("->", field("return_type", $._type))),
        choice(field("body", $.block), seq("=>", field("body", $._expression), ";"), ";"),
      ),

    parameters: ($) =>
      seq("(", commaSep(choice($.receiver, $.parameter, $.variadic_parameter)), optional(","), ")"),
    receiver: () => seq(optional(seq("borrow", optional("mut"))), "this"),
    parameter: ($) => seq(field("name", $.identifier), ":", field("type", $._type)),
    variadic_parameter: () => "...",

    type_parameters: ($) => seq("<", commaSep1($.type_parameter), optional(","), ">"),
    type_parameter: ($) =>
      choice(
        seq("region", field("name", $.identifier), optional(seq(":", sep1("+", $.identifier)))),
        seq("const", field("name", $.identifier), ":", field("type", $._type)),
        seq(field("name", alias($.identifier, $.type_identifier)), optional(seq(":", sep1("+", $._type)))),
      ),

    const_item: ($) =>
      seq(
        repeat($.attribute),
        optional($.visibility_modifier),
        "const",
        field("name", $.identifier),
        ":",
        field("type", $._type),
        optional(seq("=", field("value", $._expression))),
        ";",
      ),

    static_item: ($) =>
      seq(
        repeat($.attribute),
        optional($.visibility_modifier),
        "static",
        optional("mut"),
        field("name", $.identifier),
        ":",
        field("type", $._type),
        optional(seq("=", field("value", $._expression))),
        ";",
      ),

    struct_item: ($) =>
      seq(
        repeat($.attribute),
        optional($.visibility_modifier),
        "struct",
        field("name", alias($.identifier, $.type_identifier)),
        optional(field("type_parameters", $.type_parameters)),
        choice(field("body", $.declaration_list), ";"),
      ),
    union_item: ($) =>
      seq(
        repeat($.attribute),
        optional($.visibility_modifier),
        "union",
        optional(field("name", alias($.identifier, $.type_identifier))),
        choice(field("body", $.declaration_list), ";"),
      ),

    declaration_list: ($) =>
      seq(
        "{",
        repeat(
          choice(
            $.field_declaration,
            $.function_item,
            $.drop_block,
            $.const_item,
            $.associated_type,
            $.union_item,
          ),
        ),
        "}",
      ),

    field_declaration: ($) =>
      seq(
        repeat($.attribute),
        optional($.visibility_modifier),
        field("name", $.identifier),
        ":",
        field("type", $._type),
        ";",
      ),

    drop_block: ($) => seq("drop", $.block),

    associated_type: ($) =>
      seq(
        "type",
        field("name", alias($.identifier, $.type_identifier)),
        optional(choice(seq(":", sep1("+", $._type)), seq("=", field("type", $._type)))),
        ";",
      ),

    enum_item: ($) =>
      seq(
        repeat($.attribute),
        optional($.visibility_modifier),
        "enum",
        field("name", alias($.identifier, $.type_identifier)),
        optional(field("type_parameters", $.type_parameters)),
        choice(
          seq(":", field("type", $._type), ";"), // an imported C enum (ADR 0079)
          field("body", $.enum_variant_list),
        ),
      ),
    enum_variant_list: ($) =>
      seq("{", commaSep($.enum_variant), optional(","), repeat($.function_item), "}"),
    enum_variant: ($) =>
      seq(
        repeat($.attribute),
        field("name", $.identifier),
        optional(seq("(", commaSep($._type), optional(","), ")")),
        optional(seq("=", field("value", $._expression))),
      ),

    interface_item: ($) =>
      seq(
        repeat($.attribute),
        optional($.visibility_modifier),
        "interface",
        field("name", alias($.identifier, $.type_identifier)),
        optional(field("type_parameters", $.type_parameters)),
        optional(seq(":", sep1("+", $._type))),
        field("body", $.declaration_list),
      ),

    impl_item: ($) =>
      seq(
        repeat($.attribute),
        "impl",
        optional(field("type_parameters", $.type_parameters)),
        field("type", $._type),
        optional(seq("for", field("for", $._type))),
        field("body", $.declaration_list),
      ),

    extern_block: ($) =>
      seq(
        repeat($.attribute),
        "extern",
        $.string_literal,
        "{",
        repeat(choice($.function_item, $.struct_item, $.union_item, $.enum_item, $.const_item, $.static_item)),
        "}",
      ),

    // ---- Types ----

    _type: ($) =>
      choice(
        alias($.identifier, $.type_identifier),
        $.scoped_type_identifier,
        $.generic_type,
        $.self_type,
        $.reference_type,
        $.pointer_type,
        $.array_type,
        $.optional_type,
        $.parenthesized_type,
        $.function_type,
        $.dyn_type,
      ),

    self_type: () => "Self",
    scoped_type_identifier: ($) =>
      seq(
        field("path", choice($.identifier, $.self_type, $.scoped_type_identifier)),
        ".",
        field("name", alias($.identifier, $.type_identifier)),
      ),
    generic_type: ($) =>
      seq(
        field("type", choice(alias($.identifier, $.type_identifier), $.scoped_type_identifier, $.self_type)),
        field("type_arguments", $.type_arguments),
      ),
    type_arguments: ($) =>
      seq("<", commaSep1(choice($._type, $.const_argument)), optional(","), ">"),
    const_argument: ($) =>
      choice($.integer_literal, $.boolean_literal, seq("(", $._expression, ")"), "static"),

    reference_type: ($) =>
      prec.right(
        3,
        seq("borrow", optional("mut"), optional($.sources), field("type", $._type)),
      ),
    sources: ($) => seq("(", commaSep1(choice($.identifier, "this", "static")), ")"),
    pointer_type: ($) => prec.right(3, seq("*", optional("mut"), field("type", $._type))),
    array_type: ($) => seq("[", field("element", $._type), ";", field("length", $._expression), "]"),
    optional_type: ($) => prec(2, seq(field("type", $._type), choice("?", "??"))),
    parenthesized_type: ($) => seq("(", $._type, ")"),
    function_type: ($) =>
      prec.right(
        seq(
          repeat($.attribute),
          optional(seq("extern", $.string_literal)),
          "fn",
          "(",
          commaSep($._type),
          optional(","),
          ")",
          optional(seq("->", field("return_type", $._type))),
        ),
      ),
    dyn_type: ($) =>
      prec.right(
        seq(
          "dyn",
          optional("mut"),
          choice($.function_type, alias($.identifier, $.type_identifier), $.generic_type, $.scoped_type_identifier),
          repeat(seq("+", alias($.identifier, $.type_identifier))),
        ),
      ),

    // ---- Statements ----

    block: ($) => seq("{", repeat($._statement), optional(field("value", $._expression)), "}"),

    _statement: ($) =>
      choice(
        $.let_declaration,
        $.using_declaration,
        $.defer_statement,
        $.while_statement,
        $.for_statement,
        $.loop_statement,
        $.break_statement,
        $.continue_statement,
        $.return_statement,
        $.assignment_statement,
        $.expression_statement,
        $.empty_statement,
      ),

    empty_statement: () => ";",

    let_declaration: ($) =>
      choice(
        seq(
          choice("let", "var"),
          field("name", $.identifier),
          optional(seq(":", field("type", $._type))),
          optional(seq("=", field("value", $._expression))),
          optional(seq("else", field("else", $.block))),
          ";",
        ),
        seq(
          "let",
          field("pattern", $._pattern),
          "=",
          field("value", $._expression),
          optional(seq("else", field("else", $.block))),
          ";",
        ),
      ),

    using_declaration: ($) =>
      seq(
        "using",
        field("name", $.identifier),
        optional(seq(":", field("type", $._type))),
        "=",
        field("value", $._expression),
        ";",
      ),
    defer_statement: ($) => seq("defer", $._statement),
    while_statement: ($) =>
      seq("while", "(", field("condition", $._expression), ")", field("body", $.block)),
    for_statement: ($) =>
      seq(
        "for",
        "(",
        field("variable", $.identifier),
        "in",
        field("sequence", $._expression),
        optional(seq("..", field("end", $._expression))),
        ")",
        field("body", $.block),
      ),
    loop_statement: ($) => seq("loop", field("body", $.block)),
    break_statement: () => seq("break", ";"),
    continue_statement: () => seq("continue", ";"),
    return_statement: ($) => seq("return", optional($._expression), ";"),

    assignment_statement: ($) =>
      seq(
        field("left", $._expression),
        field(
          "operator",
          choice("=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", $.shift_right_assign),
        ),
        field("right", $._expression),
        ";",
      ),
    shift_right_assign: () => seq(">", token.immediate(">=")),

    expression_statement: ($) =>
      choice(
        seq($._expression, ";"),
        prec(1, choice($.if_expression, $.match_expression, $.unsafe_block, $.block)),
      ),

    // ---- Expressions ----

    _expression: ($) =>
      choice(
        $.identifier,
        $.self_type,
        $.this,
        $.null,
        $.boolean_literal,
        $.integer_literal,
        $.float_literal,
        $.string_literal,
        $.raw_string_literal,
        $.char_literal,
        $.generic_name,
        $.parenthesized_expression,
        $.struct_literal,
        $.array_expression,
        $.if_expression,
        $.match_expression,
        $.closure_expression,
        $.unsafe_block,
        $.block,
        $.call_expression,
        $.field_expression,
        $.index_expression,
        $.try_expression,
        $.unwrap_expression,
        $.unary_expression,
        $.reference_expression,
        $.await_expression,
        $.cast_expression,
        $.binary_expression,
        $.is_expression,
      ),

    this: () => "this",
    null: () => "null",
    boolean_literal: () => choice("true", "false"),

    generic_name: ($) =>
      prec.dynamic(
        1,
        seq(
          field("name", choice(alias($.generic_identifier, $.identifier), $.self_type)),
          field("type_arguments", $.type_arguments),
        ),
      ),
    generic_identifier: ($) => $.identifier,

    scoped_identifier: ($) =>
      prec(
        PREC.postfix,
        seq(
          field("path", choice($.identifier, $.self_type, $.generic_name, $.scoped_identifier)),
          ".",
          field("name", $.identifier),
        ),
      ),

    parenthesized_expression: ($) => seq("(", $._expression, ")"),

    struct_literal: ($) =>
      prec.dynamic(
        1,
        seq(
          field(
            "name",
            choice(
              alias($.identifier, $.type_identifier),
              $.self_type,
              alias($.scoped_type_literal_name, $.scoped_type_identifier),
              $.generic_type,
            ),
          ),
          field("body", $.field_initializer_list),
        ),
      ),
    scoped_type_literal_name: ($) =>
      seq(field("path", $.identifier), ".", field("name", alias($.identifier, $.type_identifier))),
    field_initializer_list: ($) => seq("{", commaSep($.field_initializer), optional(","), "}"),
    field_initializer: ($) => seq(field("name", $.identifier), ":", field("value", $._expression)),

    array_expression: ($) =>
      seq(
        "[",
        optional(
          choice(
            seq(commaSep1($._expression), optional(",")),
            seq($._expression, ";", field("length", $._expression)),
            seq(commaSep1($.indexed_element), optional(",")),
          ),
        ),
        "]",
      ),
    indexed_element: ($) => seq(field("index", $._expression), ":", field("value", $._expression)),

    if_expression: ($) =>
      prec.right(
        seq(
          "if",
          "(",
          field("condition", $._expression),
          ")",
          field("consequence", $._expression),
          optional(seq("else", field("alternative", $._expression))),
        ),
      ),

    match_expression: ($) =>
      seq("match", "(", field("value", $._expression), ")", field("body", $.match_block)),
    match_block: ($) => seq("{", repeat($.match_arm), "}"),
    // An arm whose value is a block ends there: `0 => { … }` then `-1 => …` is two arms, not `{ … } - 1`.
    match_arm: ($) =>
      prec.right(
        seq(
          field("pattern", $._pattern),
          optional(seq("if", "(", field("guard", $._expression), ")")),
          "=>",
          choice(
            prec.dynamic(1, seq(field("value", alias($.arm_block, $.block)), optional(","))),
            seq(field("value", $._expression), optional(",")),
          ),
        ),
      ),
    arm_block: ($) => seq("{", repeat($._statement), optional(field("value", $._expression)), "}"),

    closure_expression: ($) =>
      prec(
        PREC.closure,
        seq(
          optional("async"),
          optional("move"),
          field("parameters", $.closure_parameters),
          "=>",
          field("body", $._expression),
        ),
      ),
    closure_parameters: ($) => seq("(", commaSep($.closure_parameter), optional(","), ")"),
    closure_parameter: ($) =>
      seq(field("name", $.identifier), optional(seq(":", field("type", $._type)))),

    unsafe_block: ($) => seq("unsafe", $.block),

    call_expression: ($) =>
      prec(PREC.postfix, seq(field("function", $._expression), field("arguments", $.arguments))),
    arguments: ($) => seq("(", commaSep($._argument), optional(","), ")"),
    _argument: ($) => choice($._expression, $.mut_argument),
    mut_argument: ($) => seq("mut", $._expression),

    field_expression: ($) =>
      prec(
        PREC.postfix,
        seq(
          field("value", $._expression),
          ".",
          field("field", $.identifier),
          optional(field("type_arguments", $.type_arguments)), // `s.parseInt<u32>()`, ADR 0064 §1
        ),
      ),

    index_expression: ($) =>
      prec(
        PREC.postfix,
        seq(field("value", $._expression), "[", field("index", choice($._expression, $.range)), "]"),
      ),
    range: ($) =>
      choice(
        seq($._expression, "..", optional($._expression)),
        seq("..", optional($._expression)),
      ),

    try_expression: ($) => prec(PREC.postfix, seq($._expression, "?")),
    unwrap_expression: ($) => prec(PREC.postfix, seq($._expression, "!")),

    unary_expression: ($) =>
      prec(PREC.unary, seq(field("operator", choice("-", "!", "~", "*", "move")), $._expression)),
    reference_expression: ($) =>
      prec(PREC.unary, seq("borrow", optional("mut"), field("value", $._expression))),
    await_expression: ($) => prec(PREC.unary, seq("await", $._expression)),

    cast_expression: ($) =>
      prec.left(PREC.cast, seq(field("value", $._expression), "as", field("type", $._type))),

    is_expression: ($) =>
      prec.left(PREC.compare, seq(field("value", $._expression), "is", field("pattern", $._pattern))),

    binary_expression: ($) => {
      const table = [
        [PREC.fallback, "??", prec.right],
        [PREC.or, "||", prec.left],
        [PREC.and, "&&", prec.left],
        [PREC.compare, choice("==", "!=", "<", "<=", ">", ">="), prec.left],
        [PREC.bitor, "|", prec.left],
        [PREC.bitxor, "^", prec.left],
        [PREC.bitand, "&", prec.left],
        [PREC.shift, choice("<<", seq(">", token.immediate(">"))), prec.left],
        [PREC.additive, choice("+", "-"), prec.left],
        [PREC.multiplicative, choice("*", "/", "%"), prec.left],
      ];
      return choice(
        ...table.map(([precedence, operator, assoc]) =>
          assoc(
            precedence,
            seq(field("left", $._expression), field("operator", operator), field("right", $._expression)),
          ),
        ),
      );
    },

    // ---- Patterns (type-system.md §15.2) ----

    _pattern: ($) =>
      choice(
        $.wildcard_pattern,
        $.literal_pattern,
        $.binding_pattern,
        $.constant_pattern,
        $.range_pattern,
        $.variant_pattern,
        $.struct_pattern,
        $.slice_pattern,
        $.or_pattern,
        $.at_pattern,
        $.parenthesized_pattern,
        $.null,
        $.boolean_literal,
        $.string_literal,
      ),

    wildcard_pattern: () => "_",
    literal_pattern: ($) => choice(seq(optional("-"), $.integer_literal), $.char_literal),
    binding_pattern: ($) => seq(optional("var"), field("name", $.identifier)),
    constant_pattern: ($) => $.qualified_pattern_name,
    qualified_pattern_name: ($) =>
      seq(choice($.identifier, $.self_type), repeat1(seq(".", $.identifier))),
    range_pattern: ($) =>
      seq(
        field("low", choice($.literal_pattern, $.identifier, $.qualified_pattern_name)),
        choice("..=", ".."),
        field("high", choice($.literal_pattern, $.identifier, $.qualified_pattern_name)),
      ),
    variant_pattern: ($) =>
      seq(
        field("name", choice($.identifier, $.qualified_pattern_name)),
        "(",
        commaSep($._pattern),
        optional(","),
        ")",
      ),
    struct_pattern: ($) =>
      seq(
        field("type", choice(alias($.identifier, $.type_identifier), $.qualified_pattern_name, $.self_type)),
        "{",
        commaSep(choice($.field_pattern, $.rest_pattern)),
        optional(","),
        "}",
      ),
    field_pattern: ($) =>
      choice(seq(field("name", $.identifier), ":", field("pattern", $._pattern)), seq(optional("var"), field("name", $.identifier))),
    rest_pattern: () => "..",
    slice_pattern: ($) =>
      seq(
        "[",
        commaSep(choice($._pattern, $.rest_pattern, seq($.identifier, "@", ".."))),
        optional(","),
        "]",
      ),
    // Above `|` in an expression: after `is`, the pattern reads as far as it can (spec/grammar.md §2.1).
    or_pattern: ($) => prec.left(PREC.bitor + 1, seq(optional("|"), $._pattern, "|", $._pattern)),
    at_pattern: ($) => prec(1, seq(field("name", $.identifier), "@", field("pattern", $._pattern))),
    parenthesized_pattern: ($) => seq("(", $._pattern, ")"),

    // ---- Lexical (spec/grammar.md §1) ----

    identifier: () => /[A-Za-z_][A-Za-z0-9_]*/,

    integer_literal: () =>
      token(
        seq(
          choice(
            /[0-9][0-9_]*/,
            /0x[0-9A-Fa-f_]+/,
            /0b[01_]+/,
            /0o[0-7_]+/,
          ),
          optional(choice("i8", "i16", "i32", "i64", "isize", "u8", "u16", "u32", "u64", "usize")),
        ),
      ),
    float_literal: () =>
      token(
        seq(
          /[0-9][0-9_]*/,
          choice(
            seq(".", /[0-9][0-9_]*/, optional(/[eE][+-]?[0-9][0-9_]*/)),
            /[eE][+-]?[0-9][0-9_]*/,
          ),
          optional(choice("f32", "f64")),
        ),
      ),

    string_literal: ($) =>
      seq(
        alias(token(seq(optional("c"), '"')), '"'),
        repeat(choice($.string_content, $.escape_sequence)),
        token.immediate('"'),
      ),
    string_content: () => token.immediate(prec(1, /[^"\\\n]+/)),
    escape_sequence: () => token.immediate(/\\(u\{[0-9A-Fa-f]{1,6}\}|[nrt\\"'0{}])/),

    // raw_string_literal (`"""` … a line of only spaces or tabs and `"""`, §1.6) is src/scanner.c's: the end is the
    // first such line, which a longest-match token cannot find.

    char_literal: () => token(seq("'", choice(/[^'\\\n]/, /\\(u\{[0-9A-Fa-f]{1,6}\}|[nrt\\"'0{}])/), "'")),

    line_comment: () => token(seq("//", /[^\n]*/)),
    block_comment: () => token(seq("/*", /[^*]*\*+([^/*][^*]*\*+)*/, "/")),
  },
});
