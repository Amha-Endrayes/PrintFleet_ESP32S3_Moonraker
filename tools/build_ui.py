"""Build both self-contained dashboards. No external CDN, npm, or frontend build needed."""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
html = (root / 'ui/dashboard.html').read_text()
(root / 'server/static/index.html').write_text(html.replace('__CLOUD__', 'true'))
local = html.replace('__CLOUD__', 'false')
assert ')FLEET"' not in local
(root / 'firmware/PrintFleet/web_ui.h').write_text('#pragma once\n#include <pgmspace.h>\nconst char INDEX_HTML[] PROGMEM = R"FLEET(' + local + ')FLEET";\n')
print('Built local and VPS dashboards')
