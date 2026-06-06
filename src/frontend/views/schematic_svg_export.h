#pragma once
#include <string>

class SchematicModel;

// Exports the schematic to an SVG document (vector). The result is suitable for
// import into Inkscape (native), PowerPoint 2016+ (Insert > Picture > SVG), and
// any browser. Coordinates use the schematic's native pixel grid; `scale` lets
// the caller tune the rendered size (e.g. 2.0 for a "high-DPI" feel without
// affecting vector quality).
bool exportSchematicToSvg(const SchematicModel& sch,
                          std::string& outSvg,
                          double scale = 1.0);

// Convenience: emit SVG and write to file.
bool exportSchematicToSvgFile(const SchematicModel& sch,
                              const std::string& path,
                              double scale = 1.0);
