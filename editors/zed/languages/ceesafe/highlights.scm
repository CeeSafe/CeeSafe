; CeeSafe highlighting for Zed (editors/tree-sitter-ceesafe). Later patterns override earlier ones.

(identifier) @variable

; Types
(type_identifier) @type
(self_type) @type
((type_identifier) @type.builtin
  (#match? @type.builtin "^(i8|i16|i32|i64|isize|u8|u16|u32|u64|usize|f32|f64|bool|char|void|never|CChar)$"))
(struct_literal name: (type_identifier) @type)
(struct_pattern type: (type_identifier) @type)

; Functions
(function_item name: (identifier) @function)
(call_expression function: (identifier) @function)
(call_expression function: (field_expression field: (identifier) @function))
(call_expression function: (generic_name (identifier) @function))

; Fields, parameters, variants, constants
(field_declaration name: (identifier) @property)
(field_expression field: (identifier) @property)
(call_expression function: (field_expression field: (identifier) @function))
(field_initializer name: (identifier) @property)
(field_pattern name: (identifier) @property)
(parameter name: (identifier) @variable.parameter)
(closure_parameter name: (identifier) @variable.parameter)
(enum_variant name: (identifier) @constructor)
(variant_pattern name: (identifier) @constructor)
((identifier) @constant
  (#match? @constant "^[A-Z][A-Z0-9_]+$"))
(const_item name: (identifier) @constant)
(static_item name: (identifier) @constant)

; Attributes
(attribute "@" @attribute name: (identifier) @attribute)

; Literals
(integer_literal) @number
(float_literal) @number
(boolean_literal) @boolean
(null) @constant.builtin
(this) @variable.special
(string_literal) @string
(raw_string_literal) @string
(char_literal) @string.special
(escape_sequence) @string.escape

; Comments
(line_comment) @comment
(block_comment) @comment
((line_comment) @comment.doc
  (#match? @comment.doc "^///([^/]|$)"))

[
  "as" "async" "await" "borrow" "break" "const" "continue" "defer" "drop" "dyn" "else" "enum" "extern" "fn" "for"
  "if" "impl" "import" "in" "interface" "internal" "is" "let" "loop" "match" "move" "mut" "namespace" "private"
  "public" "region" "return" "static" "struct" "type" "union" "unsafe" "using" "var" "while"
] @keyword

[
  "=" "+=" "-=" "*=" "/=" "%=" "&=" "|=" "^=" "<<=" "==" "!=" "<" "<=" ">" ">=" "+" "-" "*" "/" "%" "&" "|" "^"
  "<<" "~" "!" "&&" "||" "??" "?" "=>" "->"
] @operator

["(" ")" "[" "]" "{" "}"] @punctuation.bracket
["," ";" ":" "." ".." "..="] @punctuation.delimiter
