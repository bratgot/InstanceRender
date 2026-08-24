# Embeds a PTX file as a byte array (MSVC caps string literals at ~16 KB, and
# PTX is much bigger):
#   extern const char* g_InstanceRenderPtx;
file(READ "${PTX_IN}" _hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bytes "${_hex}")
file(WRITE "${CPP_OUT}"
"// generated from ${PTX_IN} - do not edit
static const char s_ptx[] = { ${_bytes} 0x00 };
extern const char* g_InstanceRenderPtx;
const char* g_InstanceRenderPtx = s_ptx;
")
