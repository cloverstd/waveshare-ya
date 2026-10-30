"""The shared allowlist for initial Git import and exported source archives."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FILES = {'CMakeLists.txt', 'VERSION', 'sdkconfig.defaults', 'partitions.csv',
         'dependencies.lock', 'README.md', 'AGENTS.md', '.gitignore', '.clang-format'}
FOLDERS = {'main', 'components', 'docs', 'tools', 'tests', 'examples', 'web', 'worker', '.github'}
EXCLUDED = {'__pycache__', '.git', '.tools', 'private', 'build', 'artifacts',
            'node_modules', '.wrangler', 'dist', 'coverage', '.pytest_cache'}


def public_files(root=ROOT):
    for name in sorted(FILES):
        if (root / name).is_file():
            yield root / name
    for folder in sorted(FOLDERS):
        for p in sorted((root / folder).rglob('*')):
            if (not p.is_file() or p.is_symlink()
                    or any(part in EXCLUDED for part in p.relative_to(root).parts)
                    or p.suffix in {'.pyc', '.pem', '.key'}
                    or p.name.startswith(('.env', '.dev.vars')) or p.name == '.DS_Store'):
                continue
            yield p


if __name__ == '__main__':
    for path in public_files():
        print(path.relative_to(ROOT))
