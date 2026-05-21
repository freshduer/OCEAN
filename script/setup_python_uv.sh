#!/bin/bash
# Create an isolated Python env with uv, installing deps step by step.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

# Load uv into PATH
if [ -f "$HOME/.local/bin/env" ]; then
  # shellcheck disable=SC1091
  source "$HOME/.local/bin/env"
fi
export PATH="$HOME/.local/bin:$PATH"

if ! command -v uv >/dev/null 2>&1; then
  echo "uv not found; installing..."
  curl -LsSf https://astral.sh/uv/install.sh | sh
  # shellcheck disable=SC1091
  source "$HOME/.local/bin/env"
fi

echo "=== uv $(uv --version) ==="

# 1) Empty venv (Python 3.11 if available via uv, else system)
echo "=== [1/3] Create empty .venv ==="
if [ -d .venv ]; then
  echo ".venv already exists, reusing"
else
  uv venv .venv --python 3.11 2>/dev/null || uv venv .venv
fi
# shellcheck disable=SC1091
source .venv/bin/activate
python -c "import sys; print('Python', sys.version)"

# 2) Install packages one by one (easy to see which step fails)
PKGS=(
  numpy
  pandas
  matplotlib
  pyyaml
  scikit-learn
  seaborn
  gdown
  tomli
  pytest
)

echo "=== [2/3] Install packages with uv pip (one by one) ==="
for pkg in "${PKGS[@]}"; do
  echo "  -> $pkg"
  uv pip install "$pkg"
done

# 3) Verify imports
echo "=== [3/3] Verify imports ==="
python - <<'PY'
import importlib
mods = [
    "numpy", "pandas", "matplotlib", "yaml",
    "sklearn", "seaborn", "gdown", "tomli", "pytest",
]
for m in mods:
    importlib.import_module(m)
    print("  OK", m)
print("All Python deps OK")
PY

echo ""
echo "Done. Activate with:"
echo "  source $ROOT/.venv/bin/activate"
