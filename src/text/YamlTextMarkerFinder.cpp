#include "TextMarkerFinder.hpp"
#include <wx/sstream.h>
#include <wx/log.h>
#include "../common/stringUtils.hpp"
#include "tinyaml.hpp"
#include "../common/println.hpp"


class YamlTextMarkerFinder: public TextMarkerFinder {
public:
bool Reset (const wxString& text) override;
virtual ~YamlTextMarkerFinder () {}
};

struct TMYamlObj {
std::string key;
int index;
bool array;
TMYamlObj (bool b): key(), index(0), array(b) {}
std::string nextkey () {
if (array) return fmt::format("[{}]", index++);
else return key;
}
};

struct TMYamlSax: tinyaml::handler {
std::vector<TextMarker>& markers;
std::istream& in;
int level, lastPos, curPos;
const std::string& text;
std::vector<TMYamlObj> objstack;

TMYamlSax (std::vector<TextMarker>& m, std::istream& i, const std::string& t):
markers(m), in(i), level(-1), lastPos(0), text(t)   {}

inline void addMarker (int start, int end, const std::string& key, const std::string& value, int level) {
if (level<0 || objstack.empty()) return;
size_t column=0, line=0;
positionToXY(text, start, column, line);
markers.emplace_back(start, end, line+1, level, U(key), U(fmt::format("{}: {}", key, value)) );
}

inline int regLastPos () {
lastPos = curPos;
return lastPos;
}

inline std::string curkey () {
return objstack.empty()? "" : objstack.back().nextkey();
}

int position (size_t pos, size_t line, size_t col) override {
curPos = static_cast<int>(pos);
return CONTINUE;
}

int value (std::nullptr_t unused) override { 
int start = lastPos;
int end = regLastPos();
addMarker(start, end, curkey(), "null", level);
return CONTINUE; 
}

int value (bool val) override { 
int start = lastPos;
int end = regLastPos();
addMarker(start, end, curkey(), val?"true":"false", level);
return CONTINUE; 
}

int value (long long val) override { 
int start = lastPos;
int end = regLastPos();
addMarker(start, end, curkey(), std::to_string(val), level);
return CONTINUE; 
}

int value (unsigned long long val) override { 
int start = lastPos;
int end = regLastPos();
addMarker(start, end, curkey(), std::to_string(val), level);
return CONTINUE; 
}

int value (double val) override { 
int start = lastPos;
int end = regLastPos();
addMarker(start, end, curkey(), std::to_string(val), level);
return CONTINUE; 
}

int value (const std::string& s) override { 
int start = lastPos;
int end = regLastPos();
addMarker(start, end, curkey(), s, level);
return CONTINUE; 
}

int reference (const std::string& s) override { 
int start = lastPos;
int end = regLastPos();
addMarker(start, end, curkey(), "*"+s, level);
return CONTINUE; 
}

int alias (const std::string& s) override {
return CONTINUE;
}

int tag (const std::string& s) override {
return CONTINUE;
}

int key (const std::string& s) override { 
objstack.back().key = s;
return CONTINUE;
}

int beginList (size_t n) override { 
int start = lastPos;
int end = regLastPos();
addMarker(start, end, curkey(), "array", level);
objstack.emplace_back(true);
level++;
return CONTINUE; 
}

int endList () override { 
level--;
objstack.pop_back();
return CONTINUE; 
}

int beginMap (size_t n) override { 
int start = lastPos;
int end = regLastPos();
addMarker(start, end, curkey(), "object", level);
objstack.emplace_back(false);
level++;
return CONTINUE; 
}

int endMap () override { 
level--;
objstack.pop_back();
return CONTINUE; 
}

int beginDocument () override { 
int start = lastPos;
int end = regLastPos();
addMarker(start, end, "#Document", "object", level);
objstack.emplace_back(false);
level++;
return CONTINUE; 
}

int endDocument () override { 
level--;
objstack.pop_back();
return CONTINUE; 
}

void error (const std::exception& ex) { 
addMarker(lastPos, lastPos, "ERROR", ex.what(), 0);
}

};//TMYamlSax



bool YamlTextMarkerFinder::Reset (const wxString& text) {
std::string utfText = U(text);
std::istringstream in(utfText);
TMYamlSax sax(markers, in, utfText);
markers.clear();
tinyaml::parse(in, sax);
return true;
}

void registerYamlTextMarkerFinder () {
TextMarkerFinder::Register("yaml", [](auto&p){ return new YamlTextMarkerFinder(); });
}
