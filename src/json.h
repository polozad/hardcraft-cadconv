// Minimal JSON writer for the sidecars. No dependency, ~100 lines.
//
// Numbers go out through std::to_chars WITHOUT a precision argument, i.e. the
// SHORTEST representation that round-trips — exactly what python's json module
// emits (it uses repr()). That is what keeps `<glb>.uv.json` the same size as
// the reference one instead of three times bigger with %.17g padding, and it
// makes a text diff against the python output readable.
#pragma once
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

class Json
{
public:
  Json() { buf_.reserve(1 << 16); }

  void begin_obj() { sep(); buf_ += '{'; push(); }
  void end_obj()   { buf_ += '}'; pop(); }
  void begin_arr() { sep(); buf_ += '['; push(); }
  void end_arr()   { buf_ += ']'; pop(); }

  // A key does NOT count as a value: the comma belongs to the pair.
  void key(const char *k)
  {
    sep();
    quote(k);
    buf_ += ':';
    pending_key_ = true;
  }

  void num(double v)
  {
    sep();
    // Match python's repr() exactly, so a sidecar can be byte-compared against
    // the reference converter's: shortest round-trip digits, rendered FIXED when
    // the decimal exponent is in [-4, 16) and scientific outside it. Bare
    // to_chars picks whichever is shorter and writes -4e-04 where python writes
    // -0.0004 — same double, noisy diff.
    char sci[40];
    auto rs = std::to_chars(sci, sci + sizeof(sci), v, std::chars_format::scientific);
    *rs.ptr = '\0';
    int exp = 0;
    if (const char *e = std::strchr(sci, 'e'))
      exp = std::atoi(e + 1);
    const size_t at = buf_.size();
    if (exp >= -4 && exp < 16)
    {
      char fix[344];               // a double in fixed notation is bounded
      auto rf = std::to_chars(fix, fix + sizeof(fix), v, std::chars_format::fixed);
      buf_.append(fix, rf.ptr);
      // python prints a whole float as "0.0"; a bare "0" would come back from
      // json.load as an INT
      if (buf_.find('.', at) == std::string::npos)
        buf_ += ".0";
    }
    else
      buf_.append(sci, rs.ptr);
  }

  void num(int v)
  {
    sep();
    buf_ += std::to_string(v);
  }

  void str(const std::string &v) { sep(); quote(v.c_str()); }
  void boolean(bool v) { sep(); buf_ += v ? "true" : "false"; }
  void null() { sep(); buf_ += "null"; }

  // Convenience for the flat records the sidecars are full of.
  void kv(const char *k, double v) { key(k); num(v); }
  void kv(const char *k, int v) { key(k); num(v); }
  void kv(const char *k, const std::string &v) { key(k); str(v); }

  const std::string &text() const { return buf_; }

  // Sidecars are optional channels: a write that fails costs the channel, never
  // the run (the python does the same with a bare `except OSError`).
  bool write(const std::string &path) const
  {
    std::FILE *fh = std::fopen(path.c_str(), "wb");
    if (!fh)
      return false;
    const bool ok = std::fwrite(buf_.data(), 1, buf_.size(), fh) == buf_.size();
    std::fclose(fh);
    return ok;
  }

private:
  void push() { first_.push_back(true); pending_key_ = false; }
  void pop()
  {
    if (!first_.empty())
      first_.pop_back();
    if (!first_.empty())
      first_.back() = false;
  }

  void sep()
  {
    if (pending_key_)
    {
      pending_key_ = false;
      return;                       // the value right after "key":
    }
    if (first_.empty())
      return;
    if (!first_.back())
      buf_ += ',';
    first_.back() = false;
  }

  void quote(const char *s)
  {
    buf_ += '"';
    for (const char *p = s; *p; ++p)
    {
      // Sidecar strings are CAD names; escape what JSON requires and pass the
      // rest (including UTF-8) through untouched.
      switch (*p)
      {
      case '"':  buf_ += "\\\""; break;
      case '\\': buf_ += "\\\\"; break;
      case '\n': buf_ += "\\n"; break;
      case '\r': buf_ += "\\r"; break;
      case '\t': buf_ += "\\t"; break;
      default:
        if (static_cast<unsigned char>(*p) < 0x20)
        {
          char esc[8];
          std::snprintf(esc, sizeof(esc), "\\u%04x", *p);
          buf_ += esc;
        }
        else
          buf_ += *p;
      }
    }
    buf_ += '"';
  }

  std::string buf_;
  std::vector<bool> first_;
  bool pending_key_ = false;
};
