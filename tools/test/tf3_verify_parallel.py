"""Run eight independent TF3 engine suites with separate packages, saves and logs."""
import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
JOBS = [
    ('stats', 'tf3_verify_stats.py', []),
    ('attacks', 'tf3_verify_attacks.py', []),
    ('targets', 'tf3_verify_equips.py', []),
    ('flip', 'tf3_verify_flip.py', []),
    ('replacement', 'tf3_verify_replacement.py', ['--case', 'destroy']),
    ('battle', 'tf3_verify_replacement.py', ['--case', 'battle']),
    ('send', 'tf3_verify_replacement.py', ['--case', 'send']),
    ('negated', 'tf3_verify_replacement.py', ['--case', 'negated']),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', required=True, type=Path)
    parser.add_argument('--mods', required=True, type=Path)
    parser.add_argument('--tag', required=True)
    parser.add_argument('--suite', choices=['regression', 'replacement', 'mass', 'final', 'sword', 'sword-final', 'gladius', 'gladius-final', 'sabre', 'sabre-double', 'sabre-destination', 'venus', 'venus-effect', 'venus-continuous', 'venus-natural', 'venus-prohibition', 'venus-natural-negative', 'venus-jammer', 'venus-targeted', 'venus-phoenix', 'venus-rod', 'venus-immunity', 'jammer-set', 'venus-targeted-negative', 'venus-trap'], default='regression')
    args = parser.parse_args()
    if args.suite == 'replacement':
        JOBS[:] = [(case, 'tf3_verify_replacement.py', ['--case', case])
                   for case in ['destroy', 'battle', 'send', 'negated', 'mark', 'marked', 'double', 'unrelated']]
    elif args.suite == 'mass':
        JOBS[4:] = [(case, 'tf3_verify_replacement.py', ['--case', case])
                    for case in ['massmonster', 'massboth', 'double', 'unrelated']]
    elif args.suite == 'final':
        JOBS[4:] = [(case, 'tf3_verify_replacement.py', ['--case', case])
                    for case in ['natural', 'massboth', 'double', 'unrelated']]
    elif args.suite == 'sword':
        JOBS[4:] = [(case, 'tf3_verify_sword.py', ['--case', case])
                    for case in ['tribute', 'destroy', 'send', 'banish']]
    elif args.suite == 'sword-final':
        JOBS[3:] = [('sword_' + case, 'tf3_verify_sword.py', ['--case', case])
                    for case in ['natural', 'destroy', 'send', 'banish']]
        JOBS.append(('ebon_natural', 'tf3_verify_replacement.py', ['--case', 'natural']))
    elif args.suite == 'gladius':
        JOBS[:] = [(case, 'tf3_verify_sword.py', ['--gladius', '--case', case])
                   for case in ['deck', 'doubledeck', 'destroy', 'send', 'hand', 'banish', 'opposing']]
        JOBS.append(('sword_natural', 'tf3_verify_sword.py', ['--case', 'natural']))
    elif args.suite == 'gladius-final':
        JOBS[3:] = [(case, 'tf3_verify_sword.py', ['--gladius', '--case', case])
                    for case in ['deck', 'doubledeck', 'opposing']]
        JOBS.extend([('sword_natural', 'tf3_verify_sword.py', ['--case', 'natural']),
                     ('ebon_natural', 'tf3_verify_replacement.py', ['--case', 'natural'])])
    elif args.suite == 'sabre':
        JOBS[3:] = [(case, 'tf3_verify_sabre.py', ['--case', case])
                    for case in ['valid', 'wrong', 'opponent', 'decline']]
        JOBS.append(('sword_natural', 'tf3_verify_sword.py', ['--case', 'natural']))
    elif args.suite == 'sabre-double':
        JOBS[:] = [('double' + str(i), 'tf3_verify_sabre.py', ['--case', 'double']) for i in range(4)]
        JOBS.extend([(case, 'tf3_verify_sabre.py', ['--case', case]) for case in ['valid', 'wrong', 'opponent', 'decline']])
    elif args.suite == 'sabre-destination':
        JOBS[:] = [(case + str(i), 'tf3_verify_sabre.py', ['--case', case])
                   for case in ['macro', 'fissure'] for i in range(4)]
    elif args.suite == 'venus':
        JOBS[:] = [(case, 'tf3_verify_venus.py', ['--case', case]) for case in
                   ['spell', 'trap', 'other', 'opposing', 'disabled', 'facedown', 'absent', 'side1']]
    elif args.suite == 'venus-effect':
        JOBS[:] = [(case, 'tf3_verify_venus_effect.py', ['--case', case]) for case in
                   ['own0', 'own1', 'opp0', 'opp1', 'disabled', 'facedown', 'absent', 'no_jinzo']]
    elif args.suite == 'venus-continuous':
        JOBS[:] = [(source + '_' + state, 'tf3_verify_venus_effect.py', ['--case', source + '_' + state])
                   for source in ['jinzo', 'decree', 'canceller', 'order'] for state in ['on', 'off']]
    elif args.suite == 'venus-natural':
        JOBS[:] = [(case + str(i), 'tf3_verify_venus_natural.py', ['--case', case])
                   for case in ['protected', 'negated', 'plain', 'venus_only'] for i in range(2)]
    elif args.suite == 'venus-prohibition':
        JOBS[:] = [(case + str(i), 'tf3_verify_venus_natural.py', ['--case', case])
                   for case in ['canceller_venus', 'canceller_only'] for i in range(4)]
    elif args.suite == 'venus-natural-negative':
        JOBS[:] = [(case + str(i), 'tf3_verify_venus_natural.py', ['--case', case])
                   for case in ['protected', 'opposing', 'disabled', 'facedown'] for i in range(2)]
    elif args.suite == 'venus-jammer':
        JOBS[:] = [(case + str(i), 'tf3_verify_venus_natural.py', ['--case', case])
                   for case in ['jammer_venus', 'jammer_only'] for i in range(4)]
    elif args.suite == 'venus-targeted':
        JOBS[:] = [(case + str(i), 'tf3_verify_venus_targeted.py', ['--case', case])
                   for case in ['protected', 'unprotected'] for i in range(4)]
    elif args.suite == 'venus-phoenix':
        JOBS[:] = [(case + str(i), 'tf3_verify_venus_targeted.py', ['--case', case, '--source', 'phoenix'])
                   for case in ['protected', 'unprotected'] for i in range(4)]
    elif args.suite == 'venus-rod':
        JOBS[:] = [(case + str(i), 'tf3_verify_venus_targeted.py', ['--case', case, '--source', 'rod'])
                   for case in ['protected', 'unprotected'] for i in range(4)]
    elif args.suite == 'venus-immunity':
        JOBS[:] = [(case + str(i), 'tf3_verify_venus_immunity.py', ['--case', case])
                   for case in ['venus', 'baseline'] for i in range(4)]
    elif args.suite == 'jammer-set':
        JOBS[:] = [('set' + str(i), 'tf3_inspect_jammer_set.py', []) for i in range(8)]
    elif args.suite == 'venus-targeted-negative':
        JOBS[:] = [(case + str(i), 'tf3_verify_venus_targeted.py', ['--case', case])
                   for case in ['protected', 'unprotected', 'disabled', 'opposing'] for i in range(2)]
    elif args.suite == 'venus-trap':
        JOBS[:] = [(case + str(i), 'tf3_verify_venus_trap.py', ['--case', case])
                   for case in ['protected', 'unprotected'] for i in range(4)]
    if not args.tag.replace('_', '').replace('-', '').isalnum():
        parser.error('tag must contain only letters, numbers, underscores or hyphens')
    template = args.exe.resolve()
    if not template.is_file() or template.parent == ROOT:
        parser.error('exe must be an existing isolated test executable')
    output = ROOT / 'gen' / ('parallel_' + args.tag)
    output.mkdir(exist_ok=False)
    started = time.monotonic()
    # Complete all package copies before any process can modify a copied save/cache.
    packages = []
    for name, _, _ in JOBS:
        package = output / name
        shutil.copytree(template.parent, package,
                        ignore=shutil.ignore_patterns('jogo', 'ms0', 'logs', 'texcache', '__pycache__'))
        packages.append(package)
    print('Prepared eight independent packages; launching eight game suites.', flush=True)

    def run(index):
        name, script, extra = JOBS[index]
        tag = args.tag + '_' + name
        env = dict(os.environ, TF_TAG=tag, TF_SAVE_FROM=str(ROOT / 'ms0'),
                   TF_MODS_FROM=str(args.mods.resolve()), TF_RES='1',
                   TF_TEST_DATA=str(template.parent / 'jogo'),
                   TF_TEST_EXE=str(packages[index] / template.name))
        begin = time.monotonic()
        with (output / (name + '.log')).open('w', encoding='utf-8') as log:
            process = subprocess.Popen([sys.executable, str(ROOT / 'tools' / 'test' / script), *extra],
                                       cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT,
                                       creationflags=subprocess.CREATE_NO_WINDOW)
            print(f'{name}: started controller {process.pid}', flush=True)
            code = process.wait()
        result = dict(name=name, tag=tag, exit_code=code, seconds=round(time.monotonic() - begin, 1))
        print(json.dumps(result), flush=True)
        return result

    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        results = list(pool.map(run, range(8)))
    summary = dict(seconds=round(time.monotonic() - started, 1), results=results,
                   passed=all(item['exit_code'] == 0 for item in results))
    (output / 'results.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
    # The controllers stop their game in finally; retain evidence, not bulky copies.
    # A remaining pid file means cleanup cannot be assumed safe.
    for (name, _, _), package in zip(JOBS, packages):
        tag = args.tag + '_' + name
        if (ROOT / 'gen' / ('ctl_' + tag) / 'pid').exists():
            print(f'{name}: retaining files because a game may still be running', flush=True)
            continue
        shutil.rmtree(package)
        shutil.rmtree(ROOT / 'gen' / ('plugins_teste_' + tag), ignore_errors=True)
    print(json.dumps(summary), flush=True)
    return 0 if summary['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
