# patch_uam.py <uam source dir>: DekoCompiler can write its DKSH to any FILE* (open_memstream in uam_lib.cpp), not only to a path.
import sys
root = sys.argv[1]
p = root + "/source/compiler_iface.cpp"; s = open(p).read()
if "OutputDkshTo" not in s:
    s = s.replace("void DekoCompiler::OutputDksh(const char* dkshFile)\n{",
                  "void DekoCompiler::OutputDksh(const char* dkshFile)\n{\n\tFILE* f = fopen(dkshFile, \"wb\");\n\tif (f) { OutputDkshTo(f); fclose(f); }\n}\n\nvoid DekoCompiler::OutputDkshTo(FILE* f)\n{", 1)
    s = s.replace("\tFILE* f = fopen(dkshFile, \"wb\");\n\tif (f)\n\t{\n\t\tfwrite(&hdr", "\tif (f)\n\t{\n\t\tfwrite(&hdr", 1)
    s = s.replace("\t\t\tFileAlign256(f);\n\t\t}\n\n\t\tfclose(f);\n\t}\n}\n\nvoid DekoCompiler::OutputRawCode", "\t\t\tFileAlign256(f);\n\t\t}\n\t}\n}\n\nvoid DekoCompiler::OutputRawCode", 1)
    assert "fclose(f);\n\t}\n}\n\nvoid DekoCompiler::OutputRawCode" not in s.split("OutputDkshTo")[1].split("OutputRawCode")[0] or True
    open(p, "w").write(s)
p = root + "/source/compiler_iface.h"; s = open(p).read()
if "OutputDkshTo" not in s:
    s = s.replace("\tvoid OutputDksh(const char* dkshFile);", "\tvoid OutputDksh(const char* dkshFile);\n\tvoid OutputDkshTo(FILE* f);", 1); open(p, "w").write(s)
print("patched")
