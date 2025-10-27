#!/bin/bash
# Simple version bumping script
# Usage: ./scripts/bump_version.sh [major|minor|patch]

set -e

BUMP_TYPE="${1:-patch}"

if [ ! -f "version.txt" ]; then
    echo "Error: version.txt not found"
    exit 1
fi

CURRENT=$(cat version.txt)
echo "Current version: $CURRENT"

# Parse version (assumes semantic versioning: MAJOR.MINOR.PATCH)
if [[ ! $CURRENT =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)$ ]]; then
    echo "Error: Invalid version format in version.txt"
    echo "Expected: MAJOR.MINOR.PATCH (e.g., 1.0.2)"
    exit 1
fi

MAJOR="${BASH_REMATCH[1]}"
MINOR="${BASH_REMATCH[2]}"
PATCH="${BASH_REMATCH[3]}"

case "$BUMP_TYPE" in
    major)
        MAJOR=$((MAJOR + 1))
        MINOR=0
        PATCH=0
        ;;
    minor)
        MINOR=$((MINOR + 1))
        PATCH=0
        ;;
    patch)
        PATCH=$((PATCH + 1))
        ;;
    *)
        echo "Usage: $0 [major|minor|patch]"
        echo "  major: 1.0.0 -> 2.0.0"
        echo "  minor: 1.0.0 -> 1.1.0"
        echo "  patch: 1.0.0 -> 1.0.1 (default)"
        exit 1
        ;;
esac

NEW_VERSION="$MAJOR.$MINOR.$PATCH"
echo "$NEW_VERSION" > version.txt

echo "✓ Version bumped: $CURRENT -> $NEW_VERSION"
echo ""
echo "Next steps:"
echo "  ./scripts/release.sh $NEW_VERSION 'Your changelog here'"
