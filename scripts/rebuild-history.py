"""Reconstruct the project's history from the Crush session transcript.

Git was initialised late, so the only record of how the tree evolved is the
session database. Every file mutation in that session was made by one of a
small number of mechanisms, all of which are replayable:

  - the write / edit / multiedit tools
  - python heredocs:  python - <<'PYEOF' ... PYEOF
  - file heredocs:    cat > path <<'EOF' ... EOF
  - powershell one-liners using Get-Content / -replace / Set-Content
  - mv / cp / rm on tracked paths

This applies them in order into a scratch tree and commits after each step,
backdated to when it happened, so the result is a real git history that can be
bisected and reverted.
"""

import sqlite3, json, os, re, sys, datetime, subprocess, shutil, io

ROOT = 'C:/Users/guillaumec/Documents/winusbdisplay'
DB = os.path.join(ROOT, 'build/xcript/c.db')
OUT = os.path.join(ROOT, 'build/history')
SESSION = '0008754b-8cdb-4fff-8171-c21a84ff29e4'

HEREDOC = re.compile(
    r"(?P<prefix>[^\n]*?)<<\s*'(?P<delim>[A-Za-z0-9_]+)'[^\n]*\n"
    r"(?P<body>.*?)\n(?P=delim)(?=\s*$|\s*\n)", re.S | re.M)

PS_REPLACE = re.compile(
    r"\(Get-Content\s+(?P<src>[^\s]+)\s+-Raw\)\s*-replace\s*"
    r"'(?P<old>(?:[^']|'')*)'\s*,\s*'(?P<new>(?:[^']|'')*)'"
    r"\s*\|\s*Set-Content\s+(?P<dst>[^\s]+)")

MV = re.compile(r"(?:^|&&|;)\s*(?:git\s+)?mv\s+(?P<a>[^\s;&]+)\s+(?P<b>[^\s;&]+)")
RM = re.compile(r"(?:^|&&|;)\s*rm\s+-[rf]+\s+(?P<p>[^\s;&]+)")

TRACKED = ('src/', 'inf/', 'scripts/', 'docs/', 'AGENTS.md', 'README.md',
           '.gitignore', 'LICENSE')


def tracked(rel):
    rel = rel.replace('\\', '/').lstrip('./')
    return any(rel.startswith(t) for t in TRACKED)


def local(path):
    path = path.replace('\\', '/')
    if path.startswith(ROOT):
        path = path[len(ROOT):].lstrip('/')
    return os.path.join(OUT, path)


def git(*args, **kw):
    return subprocess.run(['git'] + list(args), cwd=OUT,
                          capture_output=True, text=True, **kw)


def run_python(body, when, log):
    r = subprocess.run([sys.executable, '-'], input=body.encode('utf-8'),
                       cwd=OUT, capture_output=True)
    if r.returncode != 0:
        tail = r.stderr.decode('utf-8', 'replace').strip().splitlines()[-1:]
        log.append((when, 'python', tail))


def apply_bash(cmd, when, log):
    touched = False
    for m in HEREDOC.finditer(cmd):
        prefix, body = m.group('prefix'), m.group('body')
        redirect = re.search(r'>>?\s*([^\s<>]+)\s*$', prefix)
        if 'python' in prefix and ' - ' in prefix + ' ':
            if 'open(' in body:
                run_python(body, when, log)
                touched = True
        elif redirect and tracked(redirect.group(1)):
            p = local(redirect.group(1))
            os.makedirs(os.path.dirname(p), exist_ok=True)
            mode = 'a' if '>>' in prefix else 'w'
            with io.open(p, mode, encoding='utf-8', newline='') as f:
                f.write(body + '\n')
            touched = True

    for m in PS_REPLACE.finditer(cmd):
        src, dst = m.group('src'), m.group('dst')
        if not tracked(src):
            continue
        p = local(src)
        if not os.path.exists(p):
            log.append((when, 'ps-missing', src))
            continue
        s = io.open(p, encoding='utf-8', newline='').read()
        old = m.group('old').replace("''", "'")
        new = m.group('new').replace("''", "'")
        try:
            s2 = re.sub(old, new.replace('$', '\\$'), s)
        except re.error:
            s2 = s.replace(old, new)
        if s2 != s:
            with io.open(local(dst), 'w', encoding='utf-8', newline='') as f:
                f.write(s2)
            touched = True

    for m in MV.finditer(cmd):
        a, b = local(m.group('a')), local(m.group('b'))
        if os.path.exists(a) and tracked(m.group('a')):
            os.makedirs(os.path.dirname(b), exist_ok=True)
            shutil.move(a, b)
            touched = True

    for m in RM.finditer(cmd):
        rel = m.group('p')
        if not tracked(rel):
            continue
        p = local(rel)
        if os.path.isdir(p):
            shutil.rmtree(p)
            touched = True
        elif os.path.exists(p):
            os.remove(p)
            touched = True

    return touched


def main():
    if os.path.isdir(OUT):
        shutil.rmtree(OUT)
    os.makedirs(OUT)
    git('init', '-q', '-b', 'reconstructed')
    git('config', 'user.name', 'session replay')
    git('config', 'user.email', 'replay@localhost')

    for seed in ('AGENT_PROMPT.md', 'LICENSE'):
        src = os.path.join(ROOT, seed)
        if os.path.exists(src):
            shutil.copy(src, os.path.join(OUT, seed))
    with io.open(os.path.join(OUT, '.gitignore'), 'w', encoding='utf-8') as f:
        f.write('build/\n.crush/\n')
    commit('Import the task specification', None, 1790327000)

    c = sqlite3.connect(DB)
    rows = c.execute(
        'select role, parts, created_at from messages '
        'where session_id=? order by created_at, id', (SESSION,)).fetchall()

    log = []
    n = 0
    for role, parts, ts in rows:
        when = datetime.datetime.fromtimestamp(ts).strftime('%H:%M:%S')
        ps = json.loads(parts)

        if role == 'user':
            txt = ' '.join(p['data'].get('text', '')
                           for p in ps if p['type'] == 'text').strip()
            if txt:
                commit('feedback: ' + txt.splitlines()[0][:60], txt, ts,
                       allow_empty=True)
            continue

        for part in ps:
            if part['type'] != 'tool_call':
                continue
            d = part['data']
            try:
                inp = json.loads(d['input'])
            except Exception:
                continue
            name, touched, desc = d['name'], False, ''

            if name == 'write':
                p = local(inp['file_path'])
                if tracked(inp['file_path'].replace(ROOT, '')):
                    os.makedirs(os.path.dirname(p), exist_ok=True)
                    with io.open(p, 'w', encoding='utf-8', newline='') as f:
                        f.write(inp.get('content', ''))
                    touched = True
                    desc = 'write ' + os.path.basename(inp['file_path'])
            elif name in ('edit', 'multiedit'):
                edits = inp.get('edits') or [inp]
                p = local(inp['file_path'])
                if os.path.exists(p):
                    s = io.open(p, encoding='utf-8', newline='').read()
                    for e in edits:
                        old, new = e.get('old_string', ''), e.get('new_string', '')
                        if old and old not in s:
                            log.append((when, 'edit-nomatch', inp['file_path']))
                            continue
                        s = (s.replace(old, new) if e.get('replace_all')
                             else s.replace(old, new, 1))
                    with io.open(p, 'w', encoding='utf-8', newline='') as f:
                        f.write(s)
                    touched = True
                    desc = 'edit ' + os.path.basename(inp['file_path'])
                else:
                    log.append((when, 'edit-missing', inp['file_path']))
            elif name == 'bash':
                desc = inp.get('description', '') or 'shell step'
                touched = apply_bash(inp.get('command', ''), when, log)

            if touched:
                n += 1
                commit(desc or 'change', None, ts)

    print('steps committed:', n)
    print('replay problems:', len(log))
    for x in log:
        print('  ', x)
    print(git('log', '--oneline').stdout.count('\n'), 'commits total')


def commit(subject, body, ts, allow_empty=False):
    git('add', '-A')
    when = datetime.datetime.fromtimestamp(ts).isoformat()
    env = dict(os.environ, GIT_AUTHOR_DATE=when, GIT_COMMITTER_DATE=when)
    args = ['commit', '-q', '-m', subject]
    if body:
        args += ['-m', body]
    if allow_empty:
        args.append('--allow-empty')
    subprocess.run(['git'] + args, cwd=OUT, capture_output=True, env=env)


main()
