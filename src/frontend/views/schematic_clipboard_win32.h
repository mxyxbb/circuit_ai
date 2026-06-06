#pragma once
#ifdef _WIN32

class SchematicModel;

// Renders the schematic to an in-memory bitmap (GDI+, anti-aliased) and places
// it on the Windows clipboard as CF_BITMAP. Works directly with chat inputs,
// PowerPoint, Word, Outlook, etc. that accept paste-as-image.
//
// `scale` controls output resolution: 1.0 = canvas units → 1 px each (small
// thumbnail); 2.0 ≈ 192-DPI feel; 3.0+ for large/print-quality.
//
// Returns false if the schematic is empty or the clipboard couldn't be
// acquired. No external dependencies (gdiplus is in the Windows SDK).
bool copySchematicImageToClipboard(const SchematicModel& sch, double scale);

#endif
