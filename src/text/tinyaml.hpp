#ifndef _____TINYAML_HPP_____
#define _____TINYAML_HPP_____
#include<iosfwd>
#include<string>
#include<stdexcept>

namespace tinyaml {

// SAX-style callback interface for tinyaml::parse(). Events are delivered
// in document order as the input is scanned; no in-memory DOM is built.
//
// Every method returns one of ABORT, CONTINUE or SKIP:
//   - ABORT stops parsing immediately; parse() returns without finishing
//     the input.
//   - CONTINUE parses normally.
//   - SKIP is only meaningful for beginDocument, beginList, beginMap and
//     key: it tells the parser to discard the corresponding element (its
//     whole content, without emitting any further event for it) and move
//     on to the next sibling. For beginList/beginMap, no matching
//     endList/endMap is emitted since the element itself was skipped. For
//     key, the value that follows the key is discarded and the enclosing
//     map continues with its next key. Returning SKIP from any other
//     method (endList, endMap, endDocument, value, tag, reference, alias)
//     has no effect, since those events have no content to skip; treat it
//     as CONTINUE.
struct handler {
enum {
ABORT = 0,
CONTINUE = 1,
SKIP = -1
};

// Called once per YAML document found in the input stream (a stream may
// contain several documents separated by "---").
virtual int beginDocument () = 0;
virtual int endDocument () = 0;

// Called for a block or flow sequence. length is the number of items if
// cheaply known ahead of time, or (size_t)-1 if unknown; tinyaml never
// pre-scans the input to compute it, so it is always (size_t)-1.
virtual int beginList (size_t length = -1) = 0;
virtual int endList () = 0;

// Called for a block or flow mapping. length follows the same convention
// as beginList and is always (size_t)-1.
virtual int beginMap (size_t length = -1) = 0;
virtual int endMap () = 0;

// Called for each key of a mapping, before the corresponding value event.
// Only plain and quoted scalar keys are supported; complex (non-scalar)
// keys are reported as a parse error.
virtual int key (const std::string& name) = 0;

// Called for each scalar value, using the overload matching the type
// resolved from the YAML core schema (plain scalars only; quoted and
// block scalars are always reported as strings unless an explicit
// "!!str"/"!!int"/... tag says otherwise).
virtual int value (const std::string& val) = 0;
virtual int value (long long val) = 0;
virtual int value (unsigned long long val) = 0;
virtual int value (double val) = 0;
virtual int value (bool val) = 0;
virtual int value (std::nullptr_t unused = nullptr) = 0;

// Called just before the value/beginList/beginMap/key event of the node
// carrying an explicit tag (e.g. "!!str", "!MyType").
virtual int tag (const std::string& name) = 0;

// Called just before the value/beginList/beginMap event of the node that
// defines an anchor (e.g. "&name"). The corresponding event still follows.
virtual int reference (const std::string& name) = 0;

// Called in place of a node's value/beginList/beginMap/key event when an
// alias ("*name") is used instead. No further event is emitted for this
// node; the handler is responsible for resolving it if needed (tinyaml
// keeps no anchor table of its own).
virtual int alias (const std::string& name) = 0;

// Called right before every other event (beginDocument/endDocument,
// beginList/endList, beginMap/endMap, key, value, tag, reference, alias)
// with the parser's current position. pos is a 0-based character offset,
// line is 1-based and column is 0-based.
//
// tinyaml buffers the whole input in memory before parsing (indentation-
// driven block structure generally requires looking ahead at following
// lines), so std::istream::tellg() on the original stream is useless for
// tracking progress during the callbacks -- this method is how you recover
// that information instead. Note that pos/line/column are relative to
// tinyaml's own view of the input, which normalizes CRLF and lone-CR line
// endings to LF before parsing; pos will therefore differ from the
// original stream's byte offset by one for every CRLF pair before that
// point.
//
// Returning SKIP has no effect (there is nothing to skip); treat it as
// CONTINUE.
virtual int position (size_t pos, size_t line, size_t column) = 0;

// Called when a parse error is thrown, right before parse() returns.
// The exception describes the error (see tinyaml::error in the
// implementation for the concrete type, which also carries line/column
// information).
virtual void error (const std::exception& e) = 0;
};

// Parses one or more YAML documents from a UTF-8 encoded stream (LF or
// CRLF line endings). beginDocument/endDocument are called once per
// document found in the stream. Returns handler::ABORT if parsing was
// aborted by the handler, handler::CONTINUE otherwise.
int parse (std::istream& input, handler& handler);

} // namespace tinyaml
#endif
