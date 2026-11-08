# 配布物に入れるライセンス表示を作る（DLL に組み込まれている他の人のライブラリの著作権表示と条文）。
# 出力: assets/SKSE/Plugins/MiniSkirt_Licenses/THIRD_PARTY_NOTICES.txt と COPYING.txt（GPL-3.0 の全文）
# ライブラリを増やしたり版を上げたりしたら、ビルドしてからもう一度実行する。
# 使い方: python -X utf8 -I tools/gen_notices.py
import pathlib
import shutil

ROOT = pathlib.Path(__file__).resolve().parent.parent
CL = ROOT / "extern/CommonLibVR"
VC = ROOT / "build-multi/vcpkg_installed/x64-windows-static-md/share"
OUT = ROOT / "assets/SKSE/Plugins/MiniSkirt_Licenses"

# (名前, URL, 条文のファイル)
LIBS = [
    ("CommonLibSSE NG (v9.0.0)", "https://github.com/alandtse/CommonLibVR", None),
    ("CommonLibSSE NG - original MIT-licensed code", "https://github.com/alandtse/CommonLibVR", CL / "licenses/LICENSE-MIT.txt"),
    ("Hacker Disassembler Engine 64 (in CommonLibSSE NG)", "", CL / "licenses/LICENSE-hde64.txt"),
    ("OpenVR SDK (in CommonLibSSE NG)", "https://github.com/ValveSoftware/openvr", CL / "extern/openvr/LICENSE"),
    ("spdlog", "https://github.com/gabime/spdlog", VC / "spdlog/copyright"),
    ("{fmt}", "https://github.com/fmtlib/fmt", VC / "fmt/copyright"),
    ("DirectXTK", "https://github.com/microsoft/DirectXTK", VC / "directxtk/copyright"),
    ("DirectXMath", "https://github.com/microsoft/DirectXMath", VC / "directxmath/copyright"),
    ("Xbyak", "https://github.com/herumi/xbyak", VC / "xbyak/copyright"),
    ("JSON for Modern C++ (nlohmann/json)", "https://github.com/nlohmann/json", VC / "nlohmann-json/copyright"),
    ("SimpleIni", "https://github.com/brofield/simpleini", VC / "SimpleIni/copyright"),
    ("toml11", "https://github.com/ToruNiina/toml11", VC / "toml11/copyright"),
    ("rapidcsv", "https://github.com/d99kris/rapidcsv", VC / "rapidcsv/copyright"),
]

HEAD = """MiniSkirt - license and third-party notices
===========================================

MiniSkirt is free software: you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version. See COPYING.txt in this folder for the full text.

MiniSkirt is built on CommonLibSSE NG, which is licensed under
GPL-3.0-or-later WITH the Modding Exception and the GPL-3.0 Linking Exception
(with Corresponding Source). The source code of MiniSkirt is available at:
  {source}

MiniSkirt.dll contains code from the libraries listed below. Their copyright
notices and license terms follow.
"""


def main(source_url="(source URL to be filled in before release)"):
    OUT.mkdir(parents=True, exist_ok=True)
    parts = [HEAD.format(source=source_url)]
    for name, url, path in LIBS:
        parts.append("\n" + "=" * 78 + f"\n{name}\n" + (f"{url}\n" if url else "") + "=" * 78 + "\n")
        if path is None:
            parts.append("Licensed under GPL-3.0-or-later WITH Modding Exception AND GPL-3.0 Linking Exception\n(with Corresponding Source). See COPYING.txt and the exceptions below.\n\n")
            parts.append((CL / "EXCEPTIONS.md").read_text(encoding="utf-8"))
        else:
            parts.append(path.read_text(encoding="utf-8", errors="replace"))
    text = "".join(parts).replace("\r\n", "\n").replace("\n", "\r\n")
    (OUT / "THIRD_PARTY_NOTICES.txt").write_bytes(text.encode("utf-8"))
    shutil.copyfile(CL / "COPYING.txt", OUT / "COPYING.txt")
    print("ok", len(LIBS), "entries")


if __name__ == "__main__":
    import sys

    main(*sys.argv[1:])
