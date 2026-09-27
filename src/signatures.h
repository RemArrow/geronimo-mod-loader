// Prologue signatures taken from Geronimo-Win64-Shipping.exe (UE 5.7.4, CL 51494982).
// The loader uses them to verify the RVAs in GML\config\GML.cfg [Offsets] and, if those no longer
// match (game update), to re-find the functions. tools/gmlcheck.exe tests both against the exe on disk.
#pragma once

namespace gml::sig {

inline constexpr const char* ProcessEvent =
    "40 55 56 57 41 54 41 55 41 56 41 57 48 81 EC ?? ?? ?? ?? 48 8D 6C 24 ?? 48 89 9D ?? ?? ?? ?? "
    "48 8B 05 ?? ?? ?? ?? 48 33 C5 48 89 85 ?? ?? ?? ?? 8B 41 08 4D 8B F0 48 8B FA 4C 8B F9 0F BA E0 1E";

inline constexpr const char* AppendString =
    "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 80 3D ?? ?? ?? ?? 00 48 8B F2 8B 19 48 8B F9 74 09 "
    "4C 8D 05 ?? ?? ?? ?? EB 16 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 4C 8B C0 C6 05 ?? ?? ?? ?? 01 8B CB";

}  // namespace gml::sig
