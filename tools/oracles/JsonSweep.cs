// The sweep that tests/support/run-pass/json-writer.cee repeats: an array holding one string per BMP scalar value
// (surrogates skipped), then every 997th astral scalar, then the fixed strings below. Prints the length and FNV-1a 64
// of the UTF-8 output.
using System.Text.Encodings.Web;
using System.Text.Json;
var options = new JsonWriterOptions { Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping };
using var buffer = new MemoryStream();
using (var writer = new Utf8JsonWriter(buffer, options))
{
    writer.WriteStartArray();
    for (var c = 0; c < 0x10000; c++)
    {
        if (c >= 0xD800 && c <= 0xDFFF) continue;
        writer.WriteStringValue(((char)c).ToString());
    }
    for (var c = 0x10000; c <= 0x10FFFF; c += 997)
    {
        writer.WriteStringValue(char.ConvertFromUtf32(c));
    }
    writer.WriteStringValue("tab\there \"q\" back\\slash \u00E9 \u20AC \U0001F600 \u2028");
    writer.WriteStringValue("ill-formed: �|�|�|�");
    writer.WriteEndArray();
}
var bytes = buffer.ToArray();
ulong h = 0xcbf29ce484222325;
foreach (var b in bytes) { h = (h ^ b) * 0x100000001b3; }
Console.WriteLine($"json sweep: {bytes.Length} bytes, fnv {h}");
