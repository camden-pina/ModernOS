#!/usr/bin/env python3
"""Generate, compile and run a real parser using Coast Bison and GNU m4."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, default=Path('/'))
    args = parser.parse_args()
    if platform.system() != 'Coast':
        parser.error('Run parser qualification inside Coast')
    root = args.runtime.resolve(strict=True)
    bison = root / 'usr/bin/bison'
    work = Path(tempfile.mkdtemp(prefix='bison-check-', dir='/tmp'))
    environment = {**os.environ, 'BISON_PKGDATADIR': str(root / 'usr/share/bison'),
        'PATH': str(root / 'usr/bin') + ':' + os.environ.get('PATH', '/usr/bin:/bin')}
    (work / 'calc.y').write_text(r'''%{
#include <ctype.h>
#include <stdio.h>
int yylex(void);
void yyerror(const char *message) { fprintf(stderr, "PARSE_ERROR %s\n", message); }
%}
%token NUMBER
%left '+' '-'
%left '*' '/'
%%
input: expression '\n' { printf("PARSE_VALUE %d\n", $1); } ;
expression: NUMBER | expression '+' expression { $$=$1+$3; }
  | expression '-' expression { $$=$1-$3; }
  | expression '*' expression { $$=$1*$3; }
  | expression '/' expression { $$=$1/$3; }
  | '(' expression ')' { $$=$2; } ;
%%
int yylex(void) {
    int ch;
    do { ch=getchar(); } while(ch==' ' || ch=='\t');
    if(ch==EOF)return 0;
    if(isdigit((unsigned char)ch)) {
        yylval=ch-'0';
        while((ch=getchar())!=EOF && isdigit((unsigned char)ch))yylval=yylval*10+ch-'0';
        if(ch!=EOF)ungetc(ch,stdin);
        return NUMBER;
    }
    return ch;
}
int main(void) { return yyparse(); }
''')

    def run(command, data=None, success=True):
        result = subprocess.run(list(map(str, command)), cwd=work, env=environment,
            input=data, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=120)
        with (work / 'check.log').open('a') as log:
            log.write('$ ' + repr(command) + '\n' + result.stdout + '\n')
        if (result.returncode == 0) != success:
            raise AssertionError(result.stdout)
        return result.stdout

    assert '3.8.2' in run([bison, '--version'])
    run([bison, '--defines=parser.h', '--report=all', '-o', 'parser.c', 'calc.y'])
    assert 'NUMBER' in (work / 'parser.h').read_text()
    assert 'State 0' in (work / 'parser.output').read_text()
    run(['gcc', '-O2', '-static', '-no-pie', 'parser.c', '-o', 'calc'])
    for expression, expected in [('1+2*3\n', 7), ('(1+2)*3\n', 9), ('24-6/2\n', 21)]:
        assert run([work / 'calc'], expression) == 'PARSE_VALUE ' + str(expected) + '\n'
    assert 'PARSE_ERROR' in run([work / 'calc'], '2+*3\n', success=False)
    run([root / 'usr/bin/yacc', '-d', 'calc.y'])
    assert (work / 'y.tab.c').is_file() and (work / 'y.tab.h').is_file()
    checks = ['GNU Bison version', 'm4 skeleton expansion, header and automaton report',
        'native GCC compiles the generated C parser', 'precedence, parentheses and three exact results',
        'syntax failure propagates', 'yacc compatibility wrapper']
    (work / 'report.json').write_text(json.dumps(dict(state='passed', checks=checks,
        bison_sha256=hashlib.sha256(bison.read_bytes()).hexdigest()), indent=2) + '\n')
    print('BISON_ALL_PASS ' + str(len(checks)) + ' report=' + str(work / 'report.json'), flush=True)


if __name__ == '__main__':
    main()
