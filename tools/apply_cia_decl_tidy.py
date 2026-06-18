from pathlib import Path
p = Path('amiga_mp3gui.c')
s = p.read_text()
s = s.replace('extern volatile struct CIA ciaa;\n', 'extern struct CIA ciaa;\n')
p.write_text(s)
