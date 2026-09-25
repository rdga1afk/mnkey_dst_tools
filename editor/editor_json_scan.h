#pragma once

// Minimal strstr-based JSON scan primitives shared by ItemEditor's and
// FactionEditor's own hand-rolled parsers (surgical-simplicity audit,
// docs/SURGICAL_SIMPLICITY_AUDIT_2026-09.md §2) -- whitespace skip,
// quoted-string extraction, and balanced open/close skip (respects
// nested quotes so a brace/bracket inside a string value doesn't
// miscount the depth).

inline const char* ejs_ws(const char* p) {
    while (*p && (*p==' '||*p=='\t'||*p=='\n'||*p=='\r')) ++p; return p;
}

inline const char* ejs_str(const char* p, char* buf, int maxlen) {
    if (*p == '"') ++p; int i = 0;
    while (*p && *p != '"' && i < maxlen-1) buf[i++] = *p++;
    buf[i] = '\0'; if (*p == '"') ++p; return p;
}

inline const char* ejs_skip(const char* p, char open, char close) {
    if (*p != open) return p; int d = 0;
    while (*p) {
        if (*p == '"') { ++p; while(*p&&*p!='"'){if(*p=='\\'&&*(p+1))++p;++p;} if(*p)++p; continue; }
        if (*p==open) ++d; else if (*p==close){if(--d==0)return p+1;} ++p;
    }
    return p;
}
