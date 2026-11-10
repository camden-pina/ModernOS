(() => {
  // codeoss-m4/stage/ports/codeoss/upstream/vscode-211a00dbedf7a224e0da0a2db3cf9cb6cb7a4bfb/src/vs/editor/common/core/position.ts
  var Position = class _Position {
    /**
     * line number (starts at 1)
     */
    lineNumber;
    /**
     * column (the first character in a line is between column 1 and column 2)
     */
    column;
    constructor(lineNumber, column) {
      this.lineNumber = lineNumber;
      this.column = column;
    }
    /**
     * Create a new position from this position.
     *
     * @param newLineNumber new line number
     * @param newColumn new column
     */
    with(newLineNumber = this.lineNumber, newColumn = this.column) {
      if (newLineNumber === this.lineNumber && newColumn === this.column) {
        return this;
      } else {
        return new _Position(newLineNumber, newColumn);
      }
    }
    /**
     * Derive a new position from this position.
     *
     * @param deltaLineNumber line number delta
     * @param deltaColumn column delta
     */
    delta(deltaLineNumber = 0, deltaColumn = 0) {
      return this.with(Math.max(1, this.lineNumber + deltaLineNumber), Math.max(1, this.column + deltaColumn));
    }
    /**
     * Test if this position equals other position
     */
    equals(other) {
      return _Position.equals(this, other);
    }
    /**
     * Test if position `a` equals position `b`
     */
    static equals(a, b) {
      if (!a && !b) {
        return true;
      }
      return !!a && !!b && a.lineNumber === b.lineNumber && a.column === b.column;
    }
    /**
     * Test if this position is before other position.
     * If the two positions are equal, the result will be false.
     */
    isBefore(other) {
      return _Position.isBefore(this, other);
    }
    /**
     * Test if position `a` is before position `b`.
     * If the two positions are equal, the result will be false.
     */
    static isBefore(a, b) {
      if (a.lineNumber < b.lineNumber) {
        return true;
      }
      if (b.lineNumber < a.lineNumber) {
        return false;
      }
      return a.column < b.column;
    }
    /**
     * Test if this position is before other position.
     * If the two positions are equal, the result will be true.
     */
    isBeforeOrEqual(other) {
      return _Position.isBeforeOrEqual(this, other);
    }
    /**
     * Test if position `a` is before position `b`.
     * If the two positions are equal, the result will be true.
     */
    static isBeforeOrEqual(a, b) {
      if (a.lineNumber < b.lineNumber) {
        return true;
      }
      if (b.lineNumber < a.lineNumber) {
        return false;
      }
      return a.column <= b.column;
    }
    /**
     * A function that compares positions, useful for sorting
     */
    static compare(a, b) {
      const aLineNumber = a.lineNumber | 0;
      const bLineNumber = b.lineNumber | 0;
      if (aLineNumber === bLineNumber) {
        const aColumn = a.column | 0;
        const bColumn = b.column | 0;
        return aColumn - bColumn;
      }
      return aLineNumber - bLineNumber;
    }
    /**
     * Clone this position.
     */
    clone() {
      return new _Position(this.lineNumber, this.column);
    }
    /**
     * Convert to a human-readable representation.
     */
    toString() {
      return "(" + this.lineNumber + "," + this.column + ")";
    }
    // ---
    /**
     * Create a `Position` from an `IPosition`.
     */
    static lift(pos) {
      return new _Position(pos.lineNumber, pos.column);
    }
    /**
     * Test if `obj` is an `IPosition`.
     */
    static isIPosition(obj) {
      return !!obj && typeof obj.lineNumber === "number" && typeof obj.column === "number";
    }
    toJSON() {
      return {
        lineNumber: this.lineNumber,
        column: this.column
      };
    }
  };

  // codeoss-m4/stage/ports/codeoss/upstream/vscode-211a00dbedf7a224e0da0a2db3cf9cb6cb7a4bfb/src/vs/editor/common/core/range.ts
  var Range = class _Range {
    /**
     * Line number on which the range starts (starts at 1).
     */
    startLineNumber;
    /**
     * Column on which the range starts in line `startLineNumber` (starts at 1).
     */
    startColumn;
    /**
     * Line number on which the range ends.
     */
    endLineNumber;
    /**
     * Column on which the range ends in line `endLineNumber`.
     */
    endColumn;
    constructor(startLineNumber, startColumn, endLineNumber, endColumn) {
      if (startLineNumber > endLineNumber || startLineNumber === endLineNumber && startColumn > endColumn) {
        this.startLineNumber = endLineNumber;
        this.startColumn = endColumn;
        this.endLineNumber = startLineNumber;
        this.endColumn = startColumn;
      } else {
        this.startLineNumber = startLineNumber;
        this.startColumn = startColumn;
        this.endLineNumber = endLineNumber;
        this.endColumn = endColumn;
      }
    }
    /**
     * Test if this range is empty.
     */
    isEmpty() {
      return _Range.isEmpty(this);
    }
    /**
     * Test if `range` is empty.
     */
    static isEmpty(range) {
      return range.startLineNumber === range.endLineNumber && range.startColumn === range.endColumn;
    }
    /**
     * Test if position is in this range. If the position is at the edges, will return true.
     */
    containsPosition(position) {
      return _Range.containsPosition(this, position);
    }
    /**
     * Test if `position` is in `range`. If the position is at the edges, will return true.
     */
    static containsPosition(range, position) {
      if (position.lineNumber < range.startLineNumber || position.lineNumber > range.endLineNumber) {
        return false;
      }
      if (position.lineNumber === range.startLineNumber && position.column < range.startColumn) {
        return false;
      }
      if (position.lineNumber === range.endLineNumber && position.column > range.endColumn) {
        return false;
      }
      return true;
    }
    /**
     * Test if `position` is in `range`. If the position is at the edges, will return false.
     * @internal
     */
    static strictContainsPosition(range, position) {
      if (position.lineNumber < range.startLineNumber || position.lineNumber > range.endLineNumber) {
        return false;
      }
      if (position.lineNumber === range.startLineNumber && position.column <= range.startColumn) {
        return false;
      }
      if (position.lineNumber === range.endLineNumber && position.column >= range.endColumn) {
        return false;
      }
      return true;
    }
    /**
     * Test if range is in this range. If the range is equal to this range, will return true.
     */
    containsRange(range) {
      return _Range.containsRange(this, range);
    }
    /**
     * Test if `otherRange` is in `range`. If the ranges are equal, will return true.
     */
    static containsRange(range, otherRange) {
      if (otherRange.startLineNumber < range.startLineNumber || otherRange.endLineNumber < range.startLineNumber) {
        return false;
      }
      if (otherRange.startLineNumber > range.endLineNumber || otherRange.endLineNumber > range.endLineNumber) {
        return false;
      }
      if (otherRange.startLineNumber === range.startLineNumber && otherRange.startColumn < range.startColumn) {
        return false;
      }
      if (otherRange.endLineNumber === range.endLineNumber && otherRange.endColumn > range.endColumn) {
        return false;
      }
      return true;
    }
    /**
     * Test if `range` is strictly in this range. `range` must start after and end before this range for the result to be true.
     */
    strictContainsRange(range) {
      return _Range.strictContainsRange(this, range);
    }
    /**
     * Test if `otherRange` is strictly in `range` (must start after, and end before). If the ranges are equal, will return false.
     */
    static strictContainsRange(range, otherRange) {
      if (otherRange.startLineNumber < range.startLineNumber || otherRange.endLineNumber < range.startLineNumber) {
        return false;
      }
      if (otherRange.startLineNumber > range.endLineNumber || otherRange.endLineNumber > range.endLineNumber) {
        return false;
      }
      if (otherRange.startLineNumber === range.startLineNumber && otherRange.startColumn <= range.startColumn) {
        return false;
      }
      if (otherRange.endLineNumber === range.endLineNumber && otherRange.endColumn >= range.endColumn) {
        return false;
      }
      return true;
    }
    /**
     * A reunion of the two ranges.
     * The smallest position will be used as the start point, and the largest one as the end point.
     */
    plusRange(range) {
      return _Range.plusRange(this, range);
    }
    /**
     * A reunion of the two ranges.
     * The smallest position will be used as the start point, and the largest one as the end point.
     */
    static plusRange(a, b) {
      let startLineNumber;
      let startColumn;
      let endLineNumber;
      let endColumn;
      if (b.startLineNumber < a.startLineNumber) {
        startLineNumber = b.startLineNumber;
        startColumn = b.startColumn;
      } else if (b.startLineNumber === a.startLineNumber) {
        startLineNumber = b.startLineNumber;
        startColumn = Math.min(b.startColumn, a.startColumn);
      } else {
        startLineNumber = a.startLineNumber;
        startColumn = a.startColumn;
      }
      if (b.endLineNumber > a.endLineNumber) {
        endLineNumber = b.endLineNumber;
        endColumn = b.endColumn;
      } else if (b.endLineNumber === a.endLineNumber) {
        endLineNumber = b.endLineNumber;
        endColumn = Math.max(b.endColumn, a.endColumn);
      } else {
        endLineNumber = a.endLineNumber;
        endColumn = a.endColumn;
      }
      return new _Range(startLineNumber, startColumn, endLineNumber, endColumn);
    }
    /**
     * A intersection of the two ranges.
     */
    intersectRanges(range) {
      return _Range.intersectRanges(this, range);
    }
    /**
     * A intersection of the two ranges.
     */
    static intersectRanges(a, b) {
      let resultStartLineNumber = a.startLineNumber;
      let resultStartColumn = a.startColumn;
      let resultEndLineNumber = a.endLineNumber;
      let resultEndColumn = a.endColumn;
      const otherStartLineNumber = b.startLineNumber;
      const otherStartColumn = b.startColumn;
      const otherEndLineNumber = b.endLineNumber;
      const otherEndColumn = b.endColumn;
      if (resultStartLineNumber < otherStartLineNumber) {
        resultStartLineNumber = otherStartLineNumber;
        resultStartColumn = otherStartColumn;
      } else if (resultStartLineNumber === otherStartLineNumber) {
        resultStartColumn = Math.max(resultStartColumn, otherStartColumn);
      }
      if (resultEndLineNumber > otherEndLineNumber) {
        resultEndLineNumber = otherEndLineNumber;
        resultEndColumn = otherEndColumn;
      } else if (resultEndLineNumber === otherEndLineNumber) {
        resultEndColumn = Math.min(resultEndColumn, otherEndColumn);
      }
      if (resultStartLineNumber > resultEndLineNumber) {
        return null;
      }
      if (resultStartLineNumber === resultEndLineNumber && resultStartColumn > resultEndColumn) {
        return null;
      }
      return new _Range(resultStartLineNumber, resultStartColumn, resultEndLineNumber, resultEndColumn);
    }
    /**
     * Test if this range equals other.
     */
    equalsRange(other) {
      return _Range.equalsRange(this, other);
    }
    /**
     * Test if range `a` equals `b`.
     */
    static equalsRange(a, b) {
      if (!a && !b) {
        return true;
      }
      return !!a && !!b && a.startLineNumber === b.startLineNumber && a.startColumn === b.startColumn && a.endLineNumber === b.endLineNumber && a.endColumn === b.endColumn;
    }
    /**
     * Return the end position (which will be after or equal to the start position)
     */
    getEndPosition() {
      return _Range.getEndPosition(this);
    }
    /**
     * Return the end position (which will be after or equal to the start position)
     */
    static getEndPosition(range) {
      return new Position(range.endLineNumber, range.endColumn);
    }
    /**
     * Return the start position (which will be before or equal to the end position)
     */
    getStartPosition() {
      return _Range.getStartPosition(this);
    }
    /**
     * Return the start position (which will be before or equal to the end position)
     */
    static getStartPosition(range) {
      return new Position(range.startLineNumber, range.startColumn);
    }
    /**
     * Transform to a user presentable string representation.
     */
    toString() {
      return "[" + this.startLineNumber + "," + this.startColumn + " -> " + this.endLineNumber + "," + this.endColumn + "]";
    }
    /**
     * Create a new range using this range's start position, and using endLineNumber and endColumn as the end position.
     */
    setEndPosition(endLineNumber, endColumn) {
      return new _Range(this.startLineNumber, this.startColumn, endLineNumber, endColumn);
    }
    /**
     * Create a new range using this range's end position, and using startLineNumber and startColumn as the start position.
     */
    setStartPosition(startLineNumber, startColumn) {
      return new _Range(startLineNumber, startColumn, this.endLineNumber, this.endColumn);
    }
    /**
     * Create a new empty range using this range's start position.
     */
    collapseToStart() {
      return _Range.collapseToStart(this);
    }
    /**
     * Create a new empty range using this range's start position.
     */
    static collapseToStart(range) {
      return new _Range(range.startLineNumber, range.startColumn, range.startLineNumber, range.startColumn);
    }
    /**
     * Create a new empty range using this range's end position.
     */
    collapseToEnd() {
      return _Range.collapseToEnd(this);
    }
    /**
     * Create a new empty range using this range's end position.
     */
    static collapseToEnd(range) {
      return new _Range(range.endLineNumber, range.endColumn, range.endLineNumber, range.endColumn);
    }
    /**
     * Moves the range by the given amount of lines.
     */
    delta(lineCount) {
      return new _Range(this.startLineNumber + lineCount, this.startColumn, this.endLineNumber + lineCount, this.endColumn);
    }
    /**
     * Test if this range starts and ends on the same line.
     */
    isSingleLine() {
      return this.startLineNumber === this.endLineNumber;
    }
    // ---
    static fromPositions(start, end = start) {
      return new _Range(start.lineNumber, start.column, end.lineNumber, end.column);
    }
    static lift(range) {
      if (!range) {
        return null;
      }
      return new _Range(range.startLineNumber, range.startColumn, range.endLineNumber, range.endColumn);
    }
    /**
     * Test if `obj` is an `IRange`.
     */
    static isIRange(obj) {
      return !!obj && typeof obj.startLineNumber === "number" && typeof obj.startColumn === "number" && typeof obj.endLineNumber === "number" && typeof obj.endColumn === "number";
    }
    /**
     * Test if the two ranges are touching in any way.
     */
    static areIntersectingOrTouching(a, b) {
      if (a.endLineNumber < b.startLineNumber || a.endLineNumber === b.startLineNumber && a.endColumn < b.startColumn) {
        return false;
      }
      if (b.endLineNumber < a.startLineNumber || b.endLineNumber === a.startLineNumber && b.endColumn < a.startColumn) {
        return false;
      }
      return true;
    }
    /**
     * Test if the two ranges are intersecting. If the ranges are touching it returns true.
     */
    static areIntersecting(a, b) {
      if (a.endLineNumber < b.startLineNumber || a.endLineNumber === b.startLineNumber && a.endColumn <= b.startColumn) {
        return false;
      }
      if (b.endLineNumber < a.startLineNumber || b.endLineNumber === a.startLineNumber && b.endColumn <= a.startColumn) {
        return false;
      }
      return true;
    }
    /**
     * Test if the two ranges are intersecting, but not touching at all.
     */
    static areOnlyIntersecting(a, b) {
      if (a.endLineNumber < b.startLineNumber - 1 || a.endLineNumber === b.startLineNumber && a.endColumn < b.startColumn - 1) {
        return false;
      }
      if (b.endLineNumber < a.startLineNumber - 1 || b.endLineNumber === a.startLineNumber && b.endColumn < a.startColumn - 1) {
        return false;
      }
      return true;
    }
    /**
     * A function that compares ranges, useful for sorting ranges
     * It will first compare ranges on the startPosition and then on the endPosition
     */
    static compareRangesUsingStarts(a, b) {
      if (a && b) {
        const aStartLineNumber = a.startLineNumber | 0;
        const bStartLineNumber = b.startLineNumber | 0;
        if (aStartLineNumber === bStartLineNumber) {
          const aStartColumn = a.startColumn | 0;
          const bStartColumn = b.startColumn | 0;
          if (aStartColumn === bStartColumn) {
            const aEndLineNumber = a.endLineNumber | 0;
            const bEndLineNumber = b.endLineNumber | 0;
            if (aEndLineNumber === bEndLineNumber) {
              const aEndColumn = a.endColumn | 0;
              const bEndColumn = b.endColumn | 0;
              return aEndColumn - bEndColumn;
            }
            return aEndLineNumber - bEndLineNumber;
          }
          return aStartColumn - bStartColumn;
        }
        return aStartLineNumber - bStartLineNumber;
      }
      const aExists = a ? 1 : 0;
      const bExists = b ? 1 : 0;
      return aExists - bExists;
    }
    /**
     * A function that compares ranges, useful for sorting ranges
     * It will first compare ranges on the endPosition and then on the startPosition
     */
    static compareRangesUsingEnds(a, b) {
      if (a.endLineNumber === b.endLineNumber) {
        if (a.endColumn === b.endColumn) {
          if (a.startLineNumber === b.startLineNumber) {
            return a.startColumn - b.startColumn;
          }
          return a.startLineNumber - b.startLineNumber;
        }
        return a.endColumn - b.endColumn;
      }
      return a.endLineNumber - b.endLineNumber;
    }
    /**
     * Test if the range spans multiple lines.
     */
    static spansMultipleLines(range) {
      return range.endLineNumber > range.startLineNumber;
    }
    toJSON() {
      return this;
    }
  };

  // codeoss-m4/stage/ports/codeoss/upstream/vscode-211a00dbedf7a224e0da0a2db3cf9cb6cb7a4bfb/src/vs/base/common/errors.ts
  var ErrorHandler = class {
    unexpectedErrorHandler;
    listeners;
    constructor() {
      this.listeners = [];
      this.unexpectedErrorHandler = function(e) {
        setTimeout(() => {
          if (e.stack) {
            if (ErrorNoTelemetry.isErrorNoTelemetry(e)) {
              throw new ErrorNoTelemetry(e.message + "\n\n" + e.stack);
            }
            throw new Error(e.message + "\n\n" + e.stack);
          }
          throw e;
        }, 0);
      };
    }
    addListener(listener) {
      this.listeners.push(listener);
      return () => {
        this._removeListener(listener);
      };
    }
    emit(e) {
      this.listeners.forEach((listener) => {
        listener(e);
      });
    }
    _removeListener(listener) {
      this.listeners.splice(this.listeners.indexOf(listener), 1);
    }
    setUnexpectedErrorHandler(newUnexpectedErrorHandler) {
      this.unexpectedErrorHandler = newUnexpectedErrorHandler;
    }
    getUnexpectedErrorHandler() {
      return this.unexpectedErrorHandler;
    }
    onUnexpectedError(e) {
      this.unexpectedErrorHandler(e);
      this.emit(e);
    }
    // For external errors, we don't want the listeners to be called
    onUnexpectedExternalError(e) {
      this.unexpectedErrorHandler(e);
    }
  };
  var errorHandler = new ErrorHandler();
  function onUnexpectedError(e) {
    if (!isCancellationError(e)) {
      errorHandler.onUnexpectedError(e);
    }
    return void 0;
  }
  var canceledName = "Canceled";
  function isCancellationError(error) {
    if (error instanceof CancellationError) {
      return true;
    }
    return error instanceof Error && error.name === canceledName && error.message === canceledName;
  }
  var CancellationError = class extends Error {
    constructor() {
      super(canceledName);
      this.name = this.message;
    }
  };
  var ErrorNoTelemetry = class _ErrorNoTelemetry extends Error {
    name;
    constructor(msg) {
      super(msg);
      this.name = "CodeExpectedError";
    }
    static fromError(err) {
      if (err instanceof _ErrorNoTelemetry) {
        return err;
      }
      const result = new _ErrorNoTelemetry();
      result.message = err.message;
      result.stack = err.stack;
      return result;
    }
    static isErrorNoTelemetry(err) {
      return err.name === "CodeExpectedError";
    }
  };
  var BugIndicatingError = class _BugIndicatingError extends Error {
    constructor(message) {
      super(message || "An unexpected bug occurred.");
      Object.setPrototypeOf(this, _BugIndicatingError.prototype);
    }
  };

  // codeoss-m4/stage/ports/codeoss/upstream/vscode-211a00dbedf7a224e0da0a2db3cf9cb6cb7a4bfb/src/vs/base/common/types.ts
  function isIterable(obj) {
    return !!obj && typeof obj[Symbol.iterator] === "function";
  }

  // codeoss-m4/stage/ports/codeoss/upstream/vscode-211a00dbedf7a224e0da0a2db3cf9cb6cb7a4bfb/src/vs/editor/common/model.ts
  var FindMatch = class {
    _findMatchBrand = void 0;
    range;
    matches;
    /**
     * @internal
     */
    constructor(range, matches) {
      this.range = range;
      this.matches = matches;
    }
  };

  // codeoss-m4/stage/ports/codeoss/upstream/vscode-211a00dbedf7a224e0da0a2db3cf9cb6cb7a4bfb/src/vs/editor/common/model/pieceTreeTextBuffer/rbTreeBase.ts
  var TreeNode = class {
    parent;
    left;
    right;
    color;
    // Piece
    piece;
    size_left;
    // size of the left subtree (not inorder)
    lf_left;
    // line feeds cnt in the left subtree (not in order)
    constructor(piece, color) {
      this.piece = piece;
      this.color = color;
      this.size_left = 0;
      this.lf_left = 0;
      this.parent = this;
      this.left = this;
      this.right = this;
    }
    next() {
      if (this.right !== SENTINEL) {
        return leftest(this.right);
      }
      let node = this;
      while (node.parent !== SENTINEL) {
        if (node.parent.left === node) {
          break;
        }
        node = node.parent;
      }
      if (node.parent === SENTINEL) {
        return SENTINEL;
      } else {
        return node.parent;
      }
    }
    prev() {
      if (this.left !== SENTINEL) {
        return righttest(this.left);
      }
      let node = this;
      while (node.parent !== SENTINEL) {
        if (node.parent.right === node) {
          break;
        }
        node = node.parent;
      }
      if (node.parent === SENTINEL) {
        return SENTINEL;
      } else {
        return node.parent;
      }
    }
    detach() {
      this.parent = null;
      this.left = null;
      this.right = null;
    }
  };
  var SENTINEL = new TreeNode(null, 0 /* Black */);
  SENTINEL.parent = SENTINEL;
  SENTINEL.left = SENTINEL;
  SENTINEL.right = SENTINEL;
  SENTINEL.color = 0 /* Black */;
  function leftest(node) {
    while (node.left !== SENTINEL) {
      node = node.left;
    }
    return node;
  }
  function righttest(node) {
    while (node.right !== SENTINEL) {
      node = node.right;
    }
    return node;
  }
  function calculateSize(node) {
    if (node === SENTINEL) {
      return 0;
    }
    return node.size_left + node.piece.length + calculateSize(node.right);
  }
  function calculateLF(node) {
    if (node === SENTINEL) {
      return 0;
    }
    return node.lf_left + node.piece.lineFeedCnt + calculateLF(node.right);
  }
  function resetSentinel() {
    SENTINEL.parent = SENTINEL;
  }
  function leftRotate(tree, x) {
    const y = x.right;
    y.size_left += x.size_left + (x.piece ? x.piece.length : 0);
    y.lf_left += x.lf_left + (x.piece ? x.piece.lineFeedCnt : 0);
    x.right = y.left;
    if (y.left !== SENTINEL) {
      y.left.parent = x;
    }
    y.parent = x.parent;
    if (x.parent === SENTINEL) {
      tree.root = y;
    } else if (x.parent.left === x) {
      x.parent.left = y;
    } else {
      x.parent.right = y;
    }
    y.left = x;
    x.parent = y;
  }
  function rightRotate(tree, y) {
    const x = y.left;
    y.left = x.right;
    if (x.right !== SENTINEL) {
      x.right.parent = y;
    }
    x.parent = y.parent;
    y.size_left -= x.size_left + (x.piece ? x.piece.length : 0);
    y.lf_left -= x.lf_left + (x.piece ? x.piece.lineFeedCnt : 0);
    if (y.parent === SENTINEL) {
      tree.root = x;
    } else if (y === y.parent.right) {
      y.parent.right = x;
    } else {
      y.parent.left = x;
    }
    x.right = y;
    y.parent = x;
  }
  function rbDelete(tree, z) {
    let x;
    let y;
    if (z.left === SENTINEL) {
      y = z;
      x = y.right;
    } else if (z.right === SENTINEL) {
      y = z;
      x = y.left;
    } else {
      y = leftest(z.right);
      x = y.right;
    }
    if (y === tree.root) {
      tree.root = x;
      x.color = 0 /* Black */;
      z.detach();
      resetSentinel();
      tree.root.parent = SENTINEL;
      return;
    }
    const yWasRed = y.color === 1 /* Red */;
    if (y === y.parent.left) {
      y.parent.left = x;
    } else {
      y.parent.right = x;
    }
    if (y === z) {
      x.parent = y.parent;
      recomputeTreeMetadata(tree, x);
    } else {
      if (y.parent === z) {
        x.parent = y;
      } else {
        x.parent = y.parent;
      }
      recomputeTreeMetadata(tree, x);
      y.left = z.left;
      y.right = z.right;
      y.parent = z.parent;
      y.color = z.color;
      if (z === tree.root) {
        tree.root = y;
      } else {
        if (z === z.parent.left) {
          z.parent.left = y;
        } else {
          z.parent.right = y;
        }
      }
      if (y.left !== SENTINEL) {
        y.left.parent = y;
      }
      if (y.right !== SENTINEL) {
        y.right.parent = y;
      }
      y.size_left = z.size_left;
      y.lf_left = z.lf_left;
      recomputeTreeMetadata(tree, y);
    }
    z.detach();
    if (x.parent.left === x) {
      const newSizeLeft = calculateSize(x);
      const newLFLeft = calculateLF(x);
      if (newSizeLeft !== x.parent.size_left || newLFLeft !== x.parent.lf_left) {
        const delta = newSizeLeft - x.parent.size_left;
        const lf_delta = newLFLeft - x.parent.lf_left;
        x.parent.size_left = newSizeLeft;
        x.parent.lf_left = newLFLeft;
        updateTreeMetadata(tree, x.parent, delta, lf_delta);
      }
    }
    recomputeTreeMetadata(tree, x.parent);
    if (yWasRed) {
      resetSentinel();
      return;
    }
    let w;
    while (x !== tree.root && x.color === 0 /* Black */) {
      if (x === x.parent.left) {
        w = x.parent.right;
        if (w.color === 1 /* Red */) {
          w.color = 0 /* Black */;
          x.parent.color = 1 /* Red */;
          leftRotate(tree, x.parent);
          w = x.parent.right;
        }
        if (w.left.color === 0 /* Black */ && w.right.color === 0 /* Black */) {
          w.color = 1 /* Red */;
          x = x.parent;
        } else {
          if (w.right.color === 0 /* Black */) {
            w.left.color = 0 /* Black */;
            w.color = 1 /* Red */;
            rightRotate(tree, w);
            w = x.parent.right;
          }
          w.color = x.parent.color;
          x.parent.color = 0 /* Black */;
          w.right.color = 0 /* Black */;
          leftRotate(tree, x.parent);
          x = tree.root;
        }
      } else {
        w = x.parent.left;
        if (w.color === 1 /* Red */) {
          w.color = 0 /* Black */;
          x.parent.color = 1 /* Red */;
          rightRotate(tree, x.parent);
          w = x.parent.left;
        }
        if (w.left.color === 0 /* Black */ && w.right.color === 0 /* Black */) {
          w.color = 1 /* Red */;
          x = x.parent;
        } else {
          if (w.left.color === 0 /* Black */) {
            w.right.color = 0 /* Black */;
            w.color = 1 /* Red */;
            leftRotate(tree, w);
            w = x.parent.left;
          }
          w.color = x.parent.color;
          x.parent.color = 0 /* Black */;
          w.left.color = 0 /* Black */;
          rightRotate(tree, x.parent);
          x = tree.root;
        }
      }
    }
    x.color = 0 /* Black */;
    resetSentinel();
  }
  function fixInsert(tree, x) {
    recomputeTreeMetadata(tree, x);
    while (x !== tree.root && x.parent.color === 1 /* Red */) {
      if (x.parent === x.parent.parent.left) {
        const y = x.parent.parent.right;
        if (y.color === 1 /* Red */) {
          x.parent.color = 0 /* Black */;
          y.color = 0 /* Black */;
          x.parent.parent.color = 1 /* Red */;
          x = x.parent.parent;
        } else {
          if (x === x.parent.right) {
            x = x.parent;
            leftRotate(tree, x);
          }
          x.parent.color = 0 /* Black */;
          x.parent.parent.color = 1 /* Red */;
          rightRotate(tree, x.parent.parent);
        }
      } else {
        const y = x.parent.parent.left;
        if (y.color === 1 /* Red */) {
          x.parent.color = 0 /* Black */;
          y.color = 0 /* Black */;
          x.parent.parent.color = 1 /* Red */;
          x = x.parent.parent;
        } else {
          if (x === x.parent.left) {
            x = x.parent;
            rightRotate(tree, x);
          }
          x.parent.color = 0 /* Black */;
          x.parent.parent.color = 1 /* Red */;
          leftRotate(tree, x.parent.parent);
        }
      }
    }
    tree.root.color = 0 /* Black */;
  }
  function updateTreeMetadata(tree, x, delta, lineFeedCntDelta) {
    while (x !== tree.root && x !== SENTINEL) {
      if (x.parent.left === x) {
        x.parent.size_left += delta;
        x.parent.lf_left += lineFeedCntDelta;
      }
      x = x.parent;
    }
  }
  function recomputeTreeMetadata(tree, x) {
    let delta = 0;
    let lf_delta = 0;
    if (x === tree.root) {
      return;
    }
    while (x !== tree.root && x === x.parent.right) {
      x = x.parent;
    }
    if (x === tree.root) {
      return;
    }
    x = x.parent;
    delta = calculateSize(x.left) - x.size_left;
    lf_delta = calculateLF(x.left) - x.lf_left;
    x.size_left += delta;
    x.lf_left += lf_delta;
    while (x !== tree.root && (delta !== 0 || lf_delta !== 0)) {
      if (x.parent.left === x) {
        x.parent.size_left += delta;
        x.parent.lf_left += lf_delta;
      }
      x = x.parent;
    }
  }

  // codeoss-m4/stage/ports/codeoss/upstream/vscode-211a00dbedf7a224e0da0a2db3cf9cb6cb7a4bfb/src/vs/base/common/arrays.ts
  var CompareResult;
  ((CompareResult2) => {
    function isLessThan(result) {
      return result < 0;
    }
    CompareResult2.isLessThan = isLessThan;
    function isLessThanOrEqual(result) {
      return result <= 0;
    }
    CompareResult2.isLessThanOrEqual = isLessThanOrEqual;
    function isGreaterThan(result) {
      return result > 0;
    }
    CompareResult2.isGreaterThan = isGreaterThan;
    function isNeitherLessOrGreaterThan(result) {
      return result === 0;
    }
    CompareResult2.isNeitherLessOrGreaterThan = isNeitherLessOrGreaterThan;
    CompareResult2.greaterThan = 1;
    CompareResult2.lessThan = -1;
    CompareResult2.neitherLessOrGreaterThan = 0;
  })(CompareResult || (CompareResult = {}));
  var CallbackIterable = class _CallbackIterable {
    constructor(iterate) {
      this.iterate = iterate;
    }
    iterate;
    static empty = new _CallbackIterable((_callback) => {
    });
    forEach(handler) {
      this.iterate((item) => {
        handler(item);
        return true;
      });
    }
    toArray() {
      const result = [];
      this.iterate((item) => {
        result.push(item);
        return true;
      });
      return result;
    }
    filter(predicate) {
      return new _CallbackIterable((cb) => this.iterate((item) => predicate(item) ? cb(item) : true));
    }
    map(mapFn) {
      return new _CallbackIterable((cb) => this.iterate((item) => cb(mapFn(item))));
    }
    some(predicate) {
      let result = false;
      this.iterate((item) => {
        result = predicate(item);
        return !result;
      });
      return result;
    }
    findFirst(predicate) {
      let result;
      this.iterate((item) => {
        if (predicate(item)) {
          result = item;
          return false;
        }
        return true;
      });
      return result;
    }
    findLast(predicate) {
      let result;
      this.iterate((item) => {
        if (predicate(item)) {
          result = item;
        }
        return true;
      });
      return result;
    }
    findLastMaxBy(comparator) {
      let result;
      let first = true;
      this.iterate((item) => {
        if (first || CompareResult.isGreaterThan(comparator(item, result))) {
          first = false;
          result = item;
        }
        return true;
      });
      return result;
    }
  };

  // codeoss-m4/stage/ports/codeoss/upstream/vscode-211a00dbedf7a224e0da0a2db3cf9cb6cb7a4bfb/src/vs/base/common/map.ts
  var LinkedMap = class {
    [Symbol.toStringTag] = "LinkedMap";
    _map;
    _head;
    _tail;
    _size;
    _state;
    constructor() {
      this._map = /* @__PURE__ */ new Map();
      this._head = void 0;
      this._tail = void 0;
      this._size = 0;
      this._state = 0;
    }
    clear() {
      this._map.clear();
      this._head = void 0;
      this._tail = void 0;
      this._size = 0;
      this._state++;
    }
    isEmpty() {
      return !this._head && !this._tail;
    }
    get size() {
      return this._size;
    }
    get first() {
      return this._head?.value;
    }
    get last() {
      return this._tail?.value;
    }
    has(key) {
      return this._map.has(key);
    }
    get(key, touch = 0 /* None */) {
      const item = this._map.get(key);
      if (!item) {
        return void 0;
      }
      if (touch !== 0 /* None */) {
        this.touch(item, touch);
      }
      return item.value;
    }
    set(key, value, touch = 0 /* None */) {
      let item = this._map.get(key);
      if (item) {
        item.value = value;
        if (touch !== 0 /* None */) {
          this.touch(item, touch);
        }
      } else {
        item = { key, value, next: void 0, previous: void 0 };
        switch (touch) {
          case 0 /* None */:
            this.addItemLast(item);
            break;
          case 1 /* AsOld */:
            this.addItemFirst(item);
            break;
          case 2 /* AsNew */:
            this.addItemLast(item);
            break;
          default:
            this.addItemLast(item);
            break;
        }
        this._map.set(key, item);
        this._size++;
      }
      return this;
    }
    delete(key) {
      return !!this.remove(key);
    }
    remove(key) {
      const item = this._map.get(key);
      if (!item) {
        return void 0;
      }
      this._map.delete(key);
      this.removeItem(item);
      this._size--;
      return item.value;
    }
    shift() {
      if (!this._head && !this._tail) {
        return void 0;
      }
      if (!this._head || !this._tail) {
        throw new Error("Invalid list");
      }
      const item = this._head;
      this._map.delete(item.key);
      this.removeItem(item);
      this._size--;
      return item.value;
    }
    forEach(callbackfn, thisArg) {
      const state = this._state;
      let current = this._head;
      while (current) {
        if (thisArg) {
          callbackfn.bind(thisArg)(current.value, current.key, this);
        } else {
          callbackfn(current.value, current.key, this);
        }
        if (this._state !== state) {
          throw new Error(`LinkedMap got modified during iteration.`);
        }
        current = current.next;
      }
    }
    keys() {
      const map = this;
      const state = this._state;
      let current = this._head;
      const iterator = {
        [Symbol.iterator]() {
          return iterator;
        },
        [Symbol.dispose]() {
        },
        next() {
          if (map._state !== state) {
            throw new Error(`LinkedMap got modified during iteration.`);
          }
          if (current) {
            const result = { value: current.key, done: false };
            current = current.next;
            return result;
          } else {
            return { value: void 0, done: true };
          }
        }
      };
      return iterator;
    }
    values() {
      const map = this;
      const state = this._state;
      let current = this._head;
      const iterator = {
        [Symbol.iterator]() {
          return iterator;
        },
        [Symbol.dispose]() {
        },
        next() {
          if (map._state !== state) {
            throw new Error(`LinkedMap got modified during iteration.`);
          }
          if (current) {
            const result = { value: current.value, done: false };
            current = current.next;
            return result;
          } else {
            return { value: void 0, done: true };
          }
        }
      };
      return iterator;
    }
    entries() {
      const map = this;
      const state = this._state;
      let current = this._head;
      const iterator = {
        [Symbol.iterator]() {
          return iterator;
        },
        [Symbol.dispose]() {
        },
        next() {
          if (map._state !== state) {
            throw new Error(`LinkedMap got modified during iteration.`);
          }
          if (current) {
            const result = { value: [current.key, current.value], done: false };
            current = current.next;
            return result;
          } else {
            return { value: void 0, done: true };
          }
        }
      };
      return iterator;
    }
    [Symbol.iterator]() {
      return this.entries();
    }
    trimOld(newSize) {
      if (newSize >= this.size) {
        return;
      }
      if (newSize === 0) {
        this.clear();
        return;
      }
      let current = this._head;
      let currentSize = this.size;
      while (current && currentSize > newSize) {
        this._map.delete(current.key);
        current = current.next;
        currentSize--;
      }
      this._head = current;
      this._size = currentSize;
      if (current) {
        current.previous = void 0;
      }
      this._state++;
    }
    trimNew(newSize) {
      if (newSize >= this.size) {
        return;
      }
      if (newSize === 0) {
        this.clear();
        return;
      }
      let current = this._tail;
      let currentSize = this.size;
      while (current && currentSize > newSize) {
        this._map.delete(current.key);
        current = current.previous;
        currentSize--;
      }
      this._tail = current;
      this._size = currentSize;
      if (current) {
        current.next = void 0;
      }
      this._state++;
    }
    addItemFirst(item) {
      if (!this._head && !this._tail) {
        this._tail = item;
      } else if (!this._head) {
        throw new Error("Invalid list");
      } else {
        item.next = this._head;
        this._head.previous = item;
      }
      this._head = item;
      this._state++;
    }
    addItemLast(item) {
      if (!this._head && !this._tail) {
        this._head = item;
      } else if (!this._tail) {
        throw new Error("Invalid list");
      } else {
        item.previous = this._tail;
        this._tail.next = item;
      }
      this._tail = item;
      this._state++;
    }
    removeItem(item) {
      if (item === this._head && item === this._tail) {
        this._head = void 0;
        this._tail = void 0;
      } else if (item === this._head) {
        if (!item.next) {
          throw new Error("Invalid list");
        }
        item.next.previous = void 0;
        this._head = item.next;
      } else if (item === this._tail) {
        if (!item.previous) {
          throw new Error("Invalid list");
        }
        item.previous.next = void 0;
        this._tail = item.previous;
      } else {
        const next2 = item.next;
        const previous = item.previous;
        if (!next2 || !previous) {
          throw new Error("Invalid list");
        }
        next2.previous = previous;
        previous.next = next2;
      }
      item.next = void 0;
      item.previous = void 0;
      this._state++;
    }
    touch(item, touch) {
      if (!this._head || !this._tail) {
        throw new Error("Invalid list");
      }
      if (touch !== 1 /* AsOld */ && touch !== 2 /* AsNew */) {
        return;
      }
      if (touch === 1 /* AsOld */) {
        if (item === this._head) {
          return;
        }
        const next2 = item.next;
        const previous = item.previous;
        if (item === this._tail) {
          previous.next = void 0;
          this._tail = previous;
        } else {
          next2.previous = previous;
          previous.next = next2;
        }
        item.previous = void 0;
        item.next = this._head;
        this._head.previous = item;
        this._head = item;
        this._state++;
      } else if (touch === 2 /* AsNew */) {
        if (item === this._tail) {
          return;
        }
        const next2 = item.next;
        const previous = item.previous;
        if (item === this._head) {
          next2.previous = void 0;
          this._head = next2;
        } else {
          next2.previous = previous;
          previous.next = next2;
        }
        item.next = void 0;
        item.previous = this._tail;
        this._tail.next = item;
        this._tail = item;
        this._state++;
      }
    }
    toJSON() {
      const data = [];
      this.forEach((value, key) => {
        data.push([key, value]);
      });
      return data;
    }
    fromJSON(data) {
      this.clear();
      for (const [key, value] of data) {
        this.set(key, value);
      }
    }
  };
  var Cache = class extends LinkedMap {
    _limit;
    _ratio;
    constructor(limit, ratio = 1) {
      super();
      this._limit = limit;
      this._ratio = Math.min(Math.max(0, ratio), 1);
    }
    get limit() {
      return this._limit;
    }
    set limit(limit) {
      this._limit = limit;
      this.checkTrim();
    }
    get ratio() {
      return this._ratio;
    }
    set ratio(ratio) {
      this._ratio = Math.min(Math.max(0, ratio), 1);
      this.checkTrim();
    }
    get(key, touch = 2 /* AsNew */) {
      return super.get(key, touch);
    }
    peek(key) {
      return super.get(key, 0 /* None */);
    }
    set(key, value) {
      super.set(key, value, 2 /* AsNew */);
      return this;
    }
    checkTrim() {
      if (this.size > this._limit) {
        this.trim(Math.round(this._limit * this._ratio));
      }
    }
  };
  var LRUCache = class extends Cache {
    constructor(limit, ratio = 1) {
      super(limit, ratio);
    }
    trim(newSize) {
      this.trimOld(newSize);
    }
    set(key, value) {
      super.set(key, value);
      this.checkTrim();
      return this;
    }
  };

  // codeoss-m4/stage/ports/codeoss/upstream/vscode-211a00dbedf7a224e0da0a2db3cf9cb6cb7a4bfb/src/vs/base/common/iterator.ts
  var Iterable;
  ((Iterable2) => {
    function is(thing) {
      return !!thing && typeof thing === "object" && typeof thing[Symbol.iterator] === "function";
    }
    Iterable2.is = is;
    const _empty = Object.freeze([]);
    function empty() {
      return _empty;
    }
    Iterable2.empty = empty;
    function* single(element) {
      yield element;
    }
    Iterable2.single = single;
    function wrap(iterableOrElement) {
      if (is(iterableOrElement)) {
        return iterableOrElement;
      } else {
        return single(iterableOrElement);
      }
    }
    Iterable2.wrap = wrap;
    function from(iterable) {
      return iterable ?? _empty;
    }
    Iterable2.from = from;
    function* reverse(array) {
      for (let i = array.length - 1; i >= 0; i--) {
        yield array[i];
      }
    }
    Iterable2.reverse = reverse;
    function isEmpty(iterable) {
      return !iterable || iterable[Symbol.iterator]().next().done === true;
    }
    Iterable2.isEmpty = isEmpty;
    function first(iterable) {
      return iterable[Symbol.iterator]().next().value;
    }
    Iterable2.first = first;
    function some(iterable, predicate) {
      let i = 0;
      for (const element of iterable) {
        if (predicate(element, i++)) {
          return true;
        }
      }
      return false;
    }
    Iterable2.some = some;
    function every(iterable, predicate) {
      let i = 0;
      for (const element of iterable) {
        if (!predicate(element, i++)) {
          return false;
        }
      }
      return true;
    }
    Iterable2.every = every;
    function find(iterable, predicate) {
      for (const element of iterable) {
        if (predicate(element)) {
          return element;
        }
      }
      return void 0;
    }
    Iterable2.find = find;
    function* filter(iterable, predicate) {
      for (const element of iterable) {
        if (predicate(element)) {
          yield element;
        }
      }
    }
    Iterable2.filter = filter;
    function* map(iterable, fn) {
      let index = 0;
      for (const element of iterable) {
        yield fn(element, index++);
      }
    }
    Iterable2.map = map;
    function* flatMap(iterable, fn) {
      let index = 0;
      for (const element of iterable) {
        yield* fn(element, index++);
      }
    }
    Iterable2.flatMap = flatMap;
    function* concat(...iterables) {
      for (const item of iterables) {
        if (isIterable(item)) {
          yield* item;
        } else {
          yield item;
        }
      }
    }
    Iterable2.concat = concat;
    function reduce(iterable, reducer, initialValue) {
      let value = initialValue;
      for (const element of iterable) {
        value = reducer(value, element);
      }
      return value;
    }
    Iterable2.reduce = reduce;
    function length(iterable) {
      let count = 0;
      for (const _ of iterable) {
        count++;
      }
      return count;
    }
    Iterable2.length = length;
    function* slice(arr, from2, to = arr.length) {
      if (from2 < -arr.length) {
        from2 = 0;
      }
      if (from2 < 0) {
        from2 += arr.length;
      }
      if (to < 0) {
        to += arr.length;
      } else if (to > arr.length) {
        to = arr.length;
      }
      for (; from2 < to; from2++) {
        yield arr[from2];
      }
    }
    Iterable2.slice = slice;
    function consume(iterable, atMost = Number.POSITIVE_INFINITY) {
      const consumed = [];
      if (atMost === 0) {
        return [consumed, iterable];
      }
      const iterator = iterable[Symbol.iterator]();
      for (let i = 0; i < atMost; i++) {
        const next2 = iterator.next();
        if (next2.done) {
          return [consumed, Iterable2.empty()];
        }
        consumed.push(next2.value);
      }
      return [consumed, { [Symbol.iterator]() {
        return iterator;
      } }];
    }
    Iterable2.consume = consume;
    async function asyncToArray(iterable) {
      const result = [];
      for await (const item of iterable) {
        result.push(item);
      }
      return result;
    }
    Iterable2.asyncToArray = asyncToArray;
    async function asyncToArrayFlat(iterable) {
      let result = [];
      for await (const item of iterable) {
        result = result.concat(item);
      }
      return result;
    }
    Iterable2.asyncToArrayFlat = asyncToArrayFlat;
  })(Iterable || (Iterable = {}));

  // codeoss-m4/stage/ports/codeoss/upstream/vscode-211a00dbedf7a224e0da0a2db3cf9cb6cb7a4bfb/src/vs/base/common/lifecycle.ts
  var TRACK_DISPOSABLES = false;
  var disposableTracker = null;
  function setDisposableTracker(tracker) {
    disposableTracker = tracker;
  }
  if (TRACK_DISPOSABLES) {
    const __is_disposable_tracked__ = "__is_disposable_tracked__";
    setDisposableTracker(new class {
      trackDisposable(x) {
        const stack = new Error("Potentially leaked disposable").stack;
        setTimeout(() => {
          if (!x[__is_disposable_tracked__]) {
            console.log(stack);
          }
        }, 3e3);
      }
      setParent(child, parent) {
        if (child && child !== Disposable.None) {
          try {
            child[__is_disposable_tracked__] = true;
          } catch {
          }
        }
      }
      markAsDisposed(disposable) {
        if (disposable && disposable !== Disposable.None) {
          try {
            disposable[__is_disposable_tracked__] = true;
          } catch {
          }
        }
      }
      markAsSingleton(disposable) {
      }
    }());
  }
  function trackDisposable(x) {
    disposableTracker?.trackDisposable(x);
    return x;
  }
  function markAsDisposed(disposable) {
    disposableTracker?.markAsDisposed(disposable);
  }
  function setParentOfDisposable(child, parent) {
    disposableTracker?.setParent(child, parent);
  }
  function setParentOfDisposables(children, parent) {
    if (!disposableTracker) {
      return;
    }
    for (const child of children) {
      disposableTracker.setParent(child, parent);
    }
  }
  function dispose(arg) {
    if (Iterable.is(arg)) {
      const errors = [];
      for (const d of arg) {
        if (d) {
          try {
            d.dispose();
          } catch (e) {
            errors.push(e);
          }
        }
      }
      if (errors.length === 1) {
        throw errors[0];
      } else if (errors.length > 1) {
        throw new AggregateError(errors, "Encountered errors while disposing of store");
      }
      return Array.isArray(arg) ? [] : arg;
    } else if (arg) {
      arg.dispose();
      return arg;
    }
  }
  function combinedDisposable(...disposables) {
    const parent = toDisposable(() => dispose(disposables));
    setParentOfDisposables(disposables, parent);
    return parent;
  }
  var FunctionDisposable = class {
    _isDisposed;
    _fn;
    constructor(fn) {
      this._isDisposed = false;
      this._fn = fn;
      trackDisposable(this);
    }
    dispose() {
      if (this._isDisposed) {
        return;
      }
      if (!this._fn) {
        throw new Error(`Unbound disposable context: Need to use an arrow function to preserve the value of this`);
      }
      this._isDisposed = true;
      markAsDisposed(this);
      this._fn();
    }
  };
  function toDisposable(fn) {
    return new FunctionDisposable(fn);
  }
  var DisposableStore = class _DisposableStore {
    static DISABLE_DISPOSED_WARNING = false;
    _toDispose = /* @__PURE__ */ new Set();
    _isDisposed = false;
    constructor() {
      trackDisposable(this);
    }
    /**
     * Dispose of all registered disposables and mark this object as disposed.
     *
     * Any future disposables added to this object will be disposed of on `add`.
     */
    dispose() {
      if (this._isDisposed) {
        return;
      }
      markAsDisposed(this);
      this._isDisposed = true;
      this.clear();
    }
    /**
     * @return `true` if this object has been disposed of.
     */
    get isDisposed() {
      return this._isDisposed;
    }
    /**
     * Dispose of all registered disposables but do not mark this object as disposed.
     */
    clear() {
      if (this._toDispose.size === 0) {
        return;
      }
      try {
        dispose(this._toDispose);
      } finally {
        this._toDispose.clear();
      }
    }
    /**
     * Add a new {@link IDisposable disposable} to the collection.
     */
    add(o) {
      if (!o || o === Disposable.None) {
        return o;
      }
      if (o === this) {
        throw new Error("Cannot register a disposable on itself!");
      }
      setParentOfDisposable(o, this);
      if (this._isDisposed) {
        if (!_DisposableStore.DISABLE_DISPOSED_WARNING) {
          console.warn(new Error("Trying to add a disposable to a DisposableStore that has already been disposed of. The added object will be leaked!").stack);
        }
      } else {
        this._toDispose.add(o);
      }
      return o;
    }
    /**
     * Deletes a disposable from store and disposes of it. This will not throw or warn and proceed to dispose the
     * disposable even when the disposable is not part in the store.
     */
    delete(o) {
      if (!o) {
        return;
      }
      if (o === this) {
        throw new Error("Cannot dispose a disposable on itself!");
      }
      this._toDispose.delete(o);
      o.dispose();
    }
    /**
     * Deletes the value from the store, but does not dispose it.
     */
    deleteAndLeak(o) {
      if (!o) {
        return;
      }
      if (this._toDispose.delete(o)) {
        setParentOfDisposable(o, null);
      }
    }
    assertNotDisposed() {
      if (this._isDisposed) {
        onUnexpectedError(new BugIndicatingError("Object disposed"));
      }
    }
  };
  var Disposable = class {
    /**
     * A disposable that does nothing when it is disposed of.
     *
     * TODO: This should not be a static property.
     */
    static None = Object.freeze({ dispose() {
    } });
    _store = new DisposableStore();
    constructor() {
      trackDisposable(this);
      setParentOfDisposable(this._store, this);
    }
    dispose() {
      markAsDisposed(this);
      this._store.dispose();
    }
    /**
     * Adds `o` to the collection of disposables managed by this object.
     */
    _register(o) {
      if (o === this) {
        throw new Error("Cannot register a disposable on itself!");
      }
      return this._store.add(o);
    }
  };

  // codeoss-m4/stage/ports/codeoss/upstream/vscode-211a00dbedf7a224e0da0a2db3cf9cb6cb7a4bfb/src/vs/base/common/linkedList.ts
  var Node = class _Node {
    static Undefined = new _Node(void 0);
    element;
    next;
    prev;
    constructor(element) {
      this.element = element;
      this.next = _Node.Undefined;
      this.prev = _Node.Undefined;
    }
  };

  // codeoss-m4/stage/ports/codeoss/upstream/vscode-211a00dbedf7a224e0da0a2db3cf9cb6cb7a4bfb/src/vs/nls.ts
  function getNLSLanguage() {
    return globalThis._VSCODE_NLS_LANGUAGE;
  }
  var isPseudo = getNLSLanguage() === "pseudo" || typeof document !== "undefined" && document.location && typeof document.location.hash === "string" && document.location.hash.indexOf("pseudo=true") >= 0;

  // codeoss-m4/stage/ports/codeoss/upstream/vscode-211a00dbedf7a224e0da0a2db3cf9cb6cb7a4bfb/src/vs/base/common/platform.ts
  var LANGUAGE_DEFAULT = "en";
  var _isWindows = false;
  var _isMacintosh = false;
  var _isLinux = false;
  var _isCoast = false;
  var _isLinuxSnap = false;
  var _isChromeOS = false;
  var _isNative = false;
  var _isWeb = false;
  var _isElectron = false;
  var _isIOS = false;
  var _isCI = false;
  var _isMobile = false;
  var _locale = void 0;
  var _language = LANGUAGE_DEFAULT;
  var _platformLocale = LANGUAGE_DEFAULT;
  var _translationsConfigFile = void 0;
  var _userAgent = void 0;
  var $globalThis = globalThis;
  var nodeProcess = void 0;
  if (typeof $globalThis.vscode !== "undefined" && typeof $globalThis.vscode.process !== "undefined") {
    nodeProcess = $globalThis.vscode.process;
  } else if (typeof process !== "undefined" && typeof process?.versions?.node === "string") {
    nodeProcess = process;
  }
  var isElectronProcess = typeof nodeProcess?.versions?.electron === "string";
  var isElectronRenderer = isElectronProcess && nodeProcess?.type === "renderer";
  var CoastHost = $globalThis.Coast;
  if (CoastHost?.protocol === 1 && CoastHost.platform === "coast" && CoastHost.runtime === "JavaScriptCore") {
    _isCoast = true;
    _isNative = true;
    _locale = LANGUAGE_DEFAULT;
    _platformLocale = LANGUAGE_DEFAULT;
  } else if (typeof nodeProcess === "object") {
    _isWindows = nodeProcess.platform === "win32";
    _isMacintosh = nodeProcess.platform === "darwin";
    _isLinux = nodeProcess.platform === "linux";
    _isLinuxSnap = _isLinux && !!nodeProcess.env["SNAP"] && !!nodeProcess.env["SNAP_REVISION"];
    _isElectron = isElectronProcess;
    _isCI = !!nodeProcess.env["CI"] || !!nodeProcess.env["BUILD_ARTIFACTSTAGINGDIRECTORY"] || !!nodeProcess.env["GITHUB_WORKSPACE"];
    _locale = LANGUAGE_DEFAULT;
    _language = LANGUAGE_DEFAULT;
    const rawNlsConfig = nodeProcess.env["VSCODE_NLS_CONFIG"];
    if (rawNlsConfig) {
      try {
        const nlsConfig = JSON.parse(rawNlsConfig);
        _locale = nlsConfig.userLocale;
        _platformLocale = nlsConfig.osLocale;
        _language = nlsConfig.resolvedLanguage || LANGUAGE_DEFAULT;
        _translationsConfigFile = nlsConfig.languagePack?.translationsConfigFile;
      } catch (e) {
      }
    }
    _isNative = true;
  } else if (typeof navigator === "object" && !isElectronRenderer) {
    _userAgent = navigator.userAgent;
    _isWindows = _userAgent.indexOf("Windows") >= 0;
    _isMacintosh = _userAgent.indexOf("Macintosh") >= 0;
    _isIOS = (_userAgent.indexOf("Macintosh") >= 0 || _userAgent.indexOf("iPad") >= 0 || _userAgent.indexOf("iPhone") >= 0) && !!navigator.maxTouchPoints && navigator.maxTouchPoints > 0;
    _isLinux = _userAgent.indexOf("Linux") >= 0;
    _isChromeOS = _userAgent.indexOf("CrOS") >= 0;
    _isMobile = _userAgent?.indexOf("Mobi") >= 0;
    _isWeb = true;
    _language = getNLSLanguage() || LANGUAGE_DEFAULT;
    _locale = navigator.language.toLowerCase();
    _platformLocale = _locale;
  } else {
    console.error("Unable to resolve platform.");
  }
  function PlatformToString(platform3) {
    switch (platform3) {
      case 0 /* Web */:
        return "Web";
      case 1 /* Mac */:
        return "Mac";
      case 2 /* Linux */:
        return "Linux";
      case 3 /* Windows */:
        return "Windows";
      case 4 /* Coast */:
        return "Coast";
    }
  }
  var _platform = 0 /* Web */;
  if (_isCoast) {
    _platform = 4 /* Coast */;
  } else if (_isMacintosh) {
    _platform = 1 /* Mac */;
  } else if (_isWindows) {
    _platform = 3 /* Windows */;
  } else if (_isLinux) {
    _platform = 2 /* Linux */;
  }
  var isWindows = _isWindows;
  var isMacintosh = _isMacintosh;
  var isCoast = _isCoast;
  var isNative = _isNative;
  var isElectron = _isElectron;
  var isWeb = _isWeb;
  var isWebWorker = _isWeb && typeof $globalThis.importScripts === "function";
  var webWorkerOrigin = isWebWorker ? $globalThis.origin : void 0;
  var platform = _platform;
  var userAgent = _userAgent;
  var language = _language;
  var Language;
  ((Language2) => {
    function value() {
      return language;
    }
    Language2.value = value;
    function isDefaultVariant() {
      if (language.length === 2) {
        return language === "en";
      } else if (language.length >= 3) {
        return language[0] === "e" && language[1] === "n" && language[2] === "-";
      } else {
        return false;
      }
    }
    Language2.isDefaultVariant = isDefaultVariant;
    function isDefault() {
      return language === "en";
    }
    Language2.isDefault = isDefault;
  })(Language || (Language = {}));
  var setTimeout0IsFaster = typeof $globalThis.postMessage === "function" && !$globalThis.importScripts;
  var setTimeout0 = (() => {
    if (setTimeout0IsFaster) {
      const pending = [];
      $globalThis.addEventListener("message", (e) => {
        if (e.data && e.data.vscodeScheduleAsyncWork) {
          for (let i = 0, len = pending.length; i < len; i++) {
            const candidate = pending[i];
            if (candidate.id === e.data.vscodeScheduleAsyncWork) {
              pending.splice(i, 1);
              candidate.callback();
              return;
            }
          }
        }
      });
      let lastId = 0;
      return (callback) => {
        const myId = ++lastId;
        pending.push({
          id: myId,
          callback
        });
        $globalThis.postMessage({ vscodeScheduleAsyncWork: myId }, "*");
      };
    }
    return (callback) => setTimeout(callback);
  })();
  var OS = _isCoast ? 4 /* Coast */ : _isMacintosh || _isIOS ? 2 /* Macintosh */ : _isWindows ? 1 /* Windows */ : 3 /* Linux */;
  var isChrome = !!(userAgent && userAgent.indexOf("Chrome") >= 0);
  var isFirefox = !!(userAgent && userAgent.indexOf("Firefox") >= 0);
  var isSafari = !!(!isChrome && (userAgent && userAgent.indexOf("Safari") >= 0));
  var isEdge = !!(userAgent && userAgent.indexOf("Edg/") >= 0);
  var isAndroid = !!(userAgent && userAgent.indexOf("Android") >= 0);
  var hasElectronUserAgent = !!(userAgent && userAgent.indexOf("Electron") >= 0);

  // codeoss-m4/stage/ports/codeoss/upstream/vscode-211a00dbedf7a224e0da0a2db3cf9cb6cb7a4bfb/src/vs/base/common/process.ts
  var safeProcess;
  var CoastHost2 = globalThis.Coast;
  var vscodeGlobal = globalThis.vscode;
  if (CoastHost2?.protocol === 1 && CoastHost2.platform === "coast" && CoastHost2.runtime === "JavaScriptCore") {
    safeProcess = {
      platform: CoastHost2.platform,
      arch: CoastHost2.arch,
      env: CoastHost2.env,
      cwd: () => CoastHost2.cwd()
    };
  } else if (typeof vscodeGlobal !== "undefined" && typeof vscodeGlobal.process !== "undefined") {
    const sandboxProcess = vscodeGlobal.process;
    safeProcess = {
      get platform() {
        return sandboxProcess.platform;
      },
      get arch() {
        return sandboxProcess.arch;
      },
      get env() {
        return sandboxProcess.env;
      },
      cwd() {
        return sandboxProcess.cwd();
      }
    };
  } else if (typeof process !== "undefined" && typeof process?.versions?.node === "string") {
    safeProcess = {
      get platform() {
        return process.platform;
      },
      get arch() {
        return process.arch;
      },
      get env() {
        return process.env;
      },
      cwd() {
        return process.env["VSCODE_CWD"] || process.cwd();
      }
    };
  } else {
    safeProcess = {
      // Supported
      get platform() {
        return isWindows ? "win32" : isMacintosh ? "darwin" : "linux";
      },
      get arch() {
        return void 0;
      },
      // Unsupported
      get env() {
        return {};
      },
      cwd() {
        return "/";
      }
    };
  }
  var cwd = safeProcess.cwd;
  var env = safeProcess.env;
  var platform2 = safeProcess.platform;
  var arch = safeProcess.arch;

  // codeoss-m4/stage/ports/codeoss/upstream/vscode-211a00dbedf7a224e0da0a2db3cf9cb6cb7a4bfb/src/vs/base/common/stopwatch.ts
  var performanceNow = globalThis.performance.now.bind(globalThis.performance);
  var StopWatch = class _StopWatch {
    _startTime;
    _stopTime;
    _now;
    static create(highResolution) {
      return new _StopWatch(highResolution);
    }
    constructor(highResolution) {
      this._now = highResolution === false ? Date.now : performanceNow;
      this._startTime = this._now();
      this._stopTime = -1;
    }
    stop() {
      this._stopTime = this._now();
    }
    reset() {
      this._startTime = this._now();
      this._stopTime = -1;
    }
    elapsed() {
      if (this._stopTime !== -1) {
        return this._stopTime - this._startTime;
      }
      return this._now() - this._startTime;
    }
  };

  // codeoss-m4/stage/ports/codeoss/upstream/vscode-211a00dbedf7a224e0da0a2db3cf9cb6cb7a4bfb/src/vs/base/common/event.ts
  var _enableDisposeWithListenerWarning = false;
  var _enableSnapshotPotentialLeakWarning = false;
  var _bufferLeakWarnCountThreshold = 100;
  var _bufferLeakWarnTimeThreshold = 6e4;
  function _isBufferLeakWarningEnabled() {
    return !!env["VSCODE_DEV"];
  }
  var Event;
  ((Event2) => {
    Event2.None = () => Disposable.None;
    function _addLeakageTraceLogic(options) {
      if (_enableSnapshotPotentialLeakWarning) {
        const { onDidAddListener: origListenerDidAdd } = options;
        const stack = Stacktrace.create();
        let count = 0;
        options.onDidAddListener = () => {
          if (++count === 2) {
            console.warn("snapshotted emitter LIKELY used public and SHOULD HAVE BEEN created with DisposableStore. snapshotted here");
            stack.print();
          }
          origListenerDidAdd?.();
        };
      }
    }
    function defer(event, flushOnListenerRemove, disposable) {
      return debounce(event, () => void 0, 0, void 0, flushOnListenerRemove ?? true, void 0, disposable);
    }
    Event2.defer = defer;
    function once(event) {
      return (listener, thisArgs = null, disposables) => {
        let didFire = false;
        let result = void 0;
        result = event((e) => {
          if (didFire) {
            return;
          } else if (result) {
            result.dispose();
          } else {
            didFire = true;
          }
          return listener.call(thisArgs, e);
        }, null, disposables);
        if (didFire) {
          result.dispose();
        }
        return result;
      };
    }
    Event2.once = once;
    function onceIf(event, condition) {
      return Event2.once(Event2.filter(event, condition));
    }
    Event2.onceIf = onceIf;
    function map(event, map2, disposable) {
      return snapshot((listener, thisArgs = null, disposables) => event((i) => listener.call(thisArgs, map2(i)), null, disposables), disposable);
    }
    Event2.map = map;
    function forEach(event, each, disposable) {
      return snapshot((listener, thisArgs = null, disposables) => event((i) => {
        each(i);
        listener.call(thisArgs, i);
      }, null, disposables), disposable);
    }
    Event2.forEach = forEach;
    function filter(event, filter2, disposable) {
      return snapshot((listener, thisArgs = null, disposables) => event((e) => filter2(e) && listener.call(thisArgs, e), null, disposables), disposable);
    }
    Event2.filter = filter;
    function signal(event) {
      return event;
    }
    Event2.signal = signal;
    function any(...events) {
      return (listener, thisArgs = null, disposables) => {
        const disposable = combinedDisposable(...events.map((event) => event((e) => listener.call(thisArgs, e))));
        return addAndReturnDisposable(disposable, disposables);
      };
    }
    Event2.any = any;
    function reduce(event, merge, initial, disposable) {
      let output = initial;
      return map(event, (e) => {
        output = merge(output, e);
        return output;
      }, disposable);
    }
    Event2.reduce = reduce;
    function snapshot(event, disposable) {
      let listener;
      const options = {
        onWillAddFirstListener() {
          listener = event(emitter.fire, emitter);
        },
        onDidRemoveLastListener() {
          listener?.dispose();
        }
      };
      if (!disposable) {
        _addLeakageTraceLogic(options);
      }
      const emitter = new Emitter(options);
      disposable?.add(emitter);
      return emitter.event;
    }
    function addAndReturnDisposable(d, store) {
      if (store instanceof Array) {
        store.push(d);
      } else if (store) {
        store.add(d);
      }
      return d;
    }
    function debounce(event, merge, delay = 100, leading = false, flushOnListenerRemove = false, leakWarningThreshold, disposable) {
      let subscription = Disposable.None;
      let output = void 0;
      let handle = void 0;
      let numDebouncedCalls = 0;
      let doFire;
      const options = {
        leakWarningThreshold,
        onWillAddFirstListener() {
          subscription = event((cur) => {
            numDebouncedCalls++;
            output = merge(output, cur);
            if (leading && !handle) {
              emitter.fire(output);
              output = void 0;
            }
            doFire = () => {
              const _output = output;
              output = void 0;
              handle = void 0;
              if (!leading || numDebouncedCalls > 1) {
                emitter.fire(_output);
              }
              numDebouncedCalls = 0;
            };
            if (typeof delay === "number") {
              if (handle) {
                clearTimeout(handle);
              }
              handle = setTimeout(doFire, delay);
            } else {
              if (handle === void 0) {
                handle = null;
                queueMicrotask(doFire);
              }
            }
          });
        },
        onWillRemoveListener() {
          if (flushOnListenerRemove && numDebouncedCalls > 0) {
            doFire?.();
          }
        },
        onDidRemoveLastListener() {
          doFire = void 0;
          subscription.dispose();
        }
      };
      if (!disposable) {
        _addLeakageTraceLogic(options);
      }
      const emitter = new Emitter(options);
      disposable?.add(emitter);
      return emitter.event;
    }
    Event2.debounce = debounce;
    function accumulate(event, delay = 0, flushOnListenerRemove, disposable) {
      return Event2.debounce(event, (last, e) => {
        if (!last) {
          return [e];
        }
        last.push(e);
        return last;
      }, delay, void 0, flushOnListenerRemove ?? true, void 0, disposable);
    }
    Event2.accumulate = accumulate;
    function throttle(event, merge, delay = 100, leading = true, trailing = true, leakWarningThreshold, disposable) {
      let subscription;
      let output = void 0;
      let handle = void 0;
      let numThrottledCalls = 0;
      const options = {
        leakWarningThreshold,
        onWillAddFirstListener() {
          subscription = event((cur) => {
            numThrottledCalls++;
            output = merge(output, cur);
            if (handle === void 0) {
              if (leading) {
                emitter.fire(output);
                output = void 0;
                numThrottledCalls = 0;
              }
              if (typeof delay === "number") {
                handle = setTimeout(() => {
                  if (trailing && numThrottledCalls > 0) {
                    emitter.fire(output);
                  }
                  output = void 0;
                  handle = void 0;
                  numThrottledCalls = 0;
                }, delay);
              } else {
                handle = 0;
                queueMicrotask(() => {
                  if (trailing && numThrottledCalls > 0) {
                    emitter.fire(output);
                  }
                  output = void 0;
                  handle = void 0;
                  numThrottledCalls = 0;
                });
              }
            }
          });
        },
        onDidRemoveLastListener() {
          subscription.dispose();
        }
      };
      if (!disposable) {
        _addLeakageTraceLogic(options);
      }
      const emitter = new Emitter(options);
      disposable?.add(emitter);
      return emitter.event;
    }
    Event2.throttle = throttle;
    function latch(event, equals2 = (a, b) => a === b, disposable) {
      let firstCall = true;
      let cache;
      return filter(event, (value) => {
        const shouldEmit = firstCall || !equals2(value, cache);
        firstCall = false;
        cache = value;
        return shouldEmit;
      }, disposable);
    }
    Event2.latch = latch;
    function split(event, isT, disposable) {
      return [
        Event2.filter(event, isT, disposable),
        Event2.filter(event, (e) => !isT(e), disposable)
      ];
    }
    Event2.split = split;
    function buffer(event, debugName, flushAfterTimeout = false, _buffer = [], disposable) {
      let buffer2 = _buffer.slice();
      let bufferLeakWarningData;
      if (_isBufferLeakWarningEnabled()) {
        bufferLeakWarningData = {
          stack: Stacktrace.create(),
          timerId: setTimeout(() => {
            if (buffer2 && buffer2.length > 0 && bufferLeakWarningData && !bufferLeakWarningData.warned) {
              bufferLeakWarningData.warned = true;
              console.warn(`[Event.buffer][${debugName}] potential LEAK detected: ${buffer2.length} events buffered for ${_bufferLeakWarnTimeThreshold / 1e3}s without being consumed. Buffered here:`);
              bufferLeakWarningData.stack.print();
            }
          }, _bufferLeakWarnTimeThreshold),
          warned: false
        };
        if (disposable) {
          disposable.add(toDisposable(() => clearTimeout(bufferLeakWarningData.timerId)));
        }
      }
      const clearLeakWarningTimer = () => {
        if (bufferLeakWarningData) {
          clearTimeout(bufferLeakWarningData.timerId);
        }
      };
      let listener = event((e) => {
        if (buffer2) {
          buffer2.push(e);
          if (_isBufferLeakWarningEnabled() && bufferLeakWarningData && !bufferLeakWarningData.warned && buffer2.length >= _bufferLeakWarnCountThreshold) {
            bufferLeakWarningData.warned = true;
            console.warn(`[Event.buffer][${debugName}] potential LEAK detected: ${buffer2.length} events buffered without being consumed. Buffered here:`);
            bufferLeakWarningData.stack.print();
          }
        } else {
          emitter.fire(e);
        }
      });
      if (disposable) {
        disposable.add(listener);
      }
      const flush = () => {
        buffer2?.forEach((e) => emitter.fire(e));
        buffer2 = null;
        clearLeakWarningTimer();
      };
      const emitter = new Emitter({
        onWillAddFirstListener() {
          if (!listener) {
            listener = event((e) => emitter.fire(e));
            if (disposable) {
              disposable.add(listener);
            }
          }
        },
        onDidAddFirstListener() {
          if (buffer2) {
            if (flushAfterTimeout) {
              setTimeout(flush);
            } else {
              flush();
            }
          }
        },
        onDidRemoveLastListener() {
          if (listener) {
            listener.dispose();
          }
          listener = null;
          clearLeakWarningTimer();
        }
      });
      if (disposable) {
        disposable.add(emitter);
      }
      return emitter.event;
    }
    Event2.buffer = buffer;
    function chain(event, sythensize) {
      const fn = (listener, thisArgs, disposables) => {
        const cs = sythensize(new ChainableSynthesis());
        return event(function(value) {
          const result = cs.evaluate(value);
          if (result !== HaltChainable) {
            listener.call(thisArgs, result);
          }
        }, void 0, disposables);
      };
      return fn;
    }
    Event2.chain = chain;
    const HaltChainable = /* @__PURE__ */ Symbol("HaltChainable");
    class ChainableSynthesis {
      steps = [];
      map(fn) {
        this.steps.push(fn);
        return this;
      }
      forEach(fn) {
        this.steps.push((v) => {
          fn(v);
          return v;
        });
        return this;
      }
      filter(fn) {
        this.steps.push((v) => fn(v) ? v : HaltChainable);
        return this;
      }
      reduce(merge, initial) {
        let last = initial;
        this.steps.push((v) => {
          last = merge(last, v);
          return last;
        });
        return this;
      }
      latch(equals2 = (a, b) => a === b) {
        let firstCall = true;
        let cache;
        this.steps.push((value) => {
          const shouldEmit = firstCall || !equals2(value, cache);
          firstCall = false;
          cache = value;
          return shouldEmit ? value : HaltChainable;
        });
        return this;
      }
      evaluate(value) {
        for (const step of this.steps) {
          value = step(value);
          if (value === HaltChainable) {
            break;
          }
        }
        return value;
      }
    }
    function fromNodeEventEmitter(emitter, eventName, map2 = (id2) => id2) {
      const fn = (...args) => result.fire(map2(...args));
      const onFirstListenerAdd = () => emitter.on(eventName, fn);
      const onLastListenerRemove = () => emitter.removeListener(eventName, fn);
      const result = new Emitter({ onWillAddFirstListener: onFirstListenerAdd, onDidRemoveLastListener: onLastListenerRemove });
      return result.event;
    }
    Event2.fromNodeEventEmitter = fromNodeEventEmitter;
    function fromDOMEventEmitter(emitter, eventName, map2 = (id2) => id2) {
      const fn = (...args) => result.fire(map2(...args));
      const onFirstListenerAdd = () => emitter.addEventListener(eventName, fn);
      const onLastListenerRemove = () => emitter.removeEventListener(eventName, fn);
      const result = new Emitter({ onWillAddFirstListener: onFirstListenerAdd, onDidRemoveLastListener: onLastListenerRemove });
      return result.event;
    }
    Event2.fromDOMEventEmitter = fromDOMEventEmitter;
    function toPromise(event, disposables) {
      let cancelRef;
      let listener;
      const promise = new Promise((resolve) => {
        listener = once(event)(resolve);
        addToDisposables(listener, disposables);
        cancelRef = () => {
          disposeAndRemove(listener, disposables);
        };
      });
      promise.cancel = cancelRef;
      if (disposables) {
        promise.finally(() => disposeAndRemove(listener, disposables));
      }
      return promise;
    }
    Event2.toPromise = toPromise;
    function forward(from, to) {
      return from((e) => to.fire(e));
    }
    Event2.forward = forward;
    function runAndSubscribe(event, handler, initial) {
      handler(initial);
      return event((e) => handler(e));
    }
    Event2.runAndSubscribe = runAndSubscribe;
    class EmitterObserver {
      constructor(_observable, store) {
        this._observable = _observable;
        const options = {
          onWillAddFirstListener: () => {
            _observable.addObserver(this);
            this._observable.reportChanges();
          },
          onDidRemoveLastListener: () => {
            _observable.removeObserver(this);
          }
        };
        if (!store) {
          _addLeakageTraceLogic(options);
        }
        this.emitter = new Emitter(options);
        if (store) {
          store.add(this.emitter);
        }
      }
      _observable;
      emitter;
      _counter = 0;
      _hasChanged = false;
      beginUpdate(_observable) {
        this._counter++;
      }
      handlePossibleChange(_observable) {
      }
      handleChange(_observable, _change) {
        this._hasChanged = true;
      }
      endUpdate(_observable) {
        this._counter--;
        if (this._counter === 0) {
          this._observable.reportChanges();
          if (this._hasChanged) {
            this._hasChanged = false;
            this.emitter.fire(this._observable.get());
          }
        }
      }
    }
    function fromObservable(obs, store) {
      const observer = new EmitterObserver(obs, store);
      return observer.emitter.event;
    }
    Event2.fromObservable = fromObservable;
    function fromObservableLight(observable) {
      return (listener, thisArgs, disposables) => {
        let count = 0;
        let didChange = false;
        const observer = {
          beginUpdate() {
            count++;
          },
          endUpdate() {
            count--;
            if (count === 0) {
              observable.reportChanges();
              if (didChange) {
                didChange = false;
                listener.call(thisArgs);
              }
            }
          },
          handlePossibleChange() {
          },
          handleChange() {
            didChange = true;
          }
        };
        observable.addObserver(observer);
        observable.reportChanges();
        const disposable = {
          dispose() {
            observable.removeObserver(observer);
          }
        };
        addToDisposables(disposable, disposables);
        return disposable;
      };
    }
    Event2.fromObservableLight = fromObservableLight;
  })(Event || (Event = {}));
  var EventProfiling = class _EventProfiling {
    static all = /* @__PURE__ */ new Set();
    static _idPool = 0;
    name;
    listenerCount = 0;
    invocationCount = 0;
    elapsedOverall = 0;
    durations = [];
    _stopWatch;
    constructor(name) {
      this.name = `${name}_${_EventProfiling._idPool++}`;
      _EventProfiling.all.add(this);
    }
    start(listenerCount) {
      this._stopWatch = new StopWatch();
      this.listenerCount = listenerCount;
    }
    stop() {
      if (this._stopWatch) {
        const elapsed = this._stopWatch.elapsed();
        this.durations.push(elapsed);
        this.elapsedOverall += elapsed;
        this.invocationCount += 1;
        this._stopWatch = void 0;
      }
    }
  };
  var _globalLeakWarningThreshold = -1;
  var leakageMonitorId = 1;
  function nextLeakageMonitorName() {
    return (leakageMonitorId++).toString(16).padStart(3, "0");
  }
  var LeakageMonitor = class {
    constructor(_errorHandler, threshold, name = nextLeakageMonitorName()) {
      this._errorHandler = _errorHandler;
      this.threshold = threshold;
      this.name = name;
    }
    _errorHandler;
    threshold;
    name;
    _stacks;
    _warnCountdown = 0;
    dispose() {
      this._stacks?.clear();
    }
    check(stack, listenerCount) {
      const threshold = this.threshold;
      if (threshold <= 0 || listenerCount < threshold) {
        return void 0;
      }
      if (!this._stacks) {
        this._stacks = /* @__PURE__ */ new Map();
      }
      const stackKey = stack.value;
      const count = this._stacks.get(stackKey) || 0;
      this._stacks.set(stackKey, count + 1);
      this._warnCountdown -= 1;
      if (this._warnCountdown <= 0) {
        this._warnCountdown = threshold * 0.5;
        const [topStack, topCount] = this.getMostFrequentStack();
        const emitterName = /^[0-9a-f]+$/i.test(this.name) ? void 0 : this.name;
        const message = `[${this.name}] potential listener LEAK detected, having ${listenerCount} listeners already. MOST frequent listener (${topCount}):`;
        console.warn(message);
        console.warn(topStack);
        const kind = topCount / listenerCount > 0.3 ? "dominated" : "popular";
        const error = new ListenerLeakError(kind, message, topStack, listenerCount, emitterName);
        this._errorHandler(error);
      }
      return () => {
        const count2 = this._stacks.get(stackKey) || 0;
        if (count2 <= 1) {
          this._stacks.delete(stackKey);
        } else {
          this._stacks.set(stackKey, count2 - 1);
        }
      };
    }
    getMostFrequentStack() {
      if (!this._stacks) {
        return void 0;
      }
      let topStack;
      let topCount = 0;
      for (const [stack, count] of this._stacks) {
        if (!topStack || topCount < count) {
          topStack = [stack, count];
          topCount = count;
        }
      }
      return topStack;
    }
  };
  var Stacktrace = class _Stacktrace {
    constructor(value) {
      this.value = value;
    }
    value;
    static create() {
      const err = new Error();
      return new _Stacktrace(err.stack ?? "");
    }
    print() {
      console.warn(this.value.split("\n").slice(2).join("\n"));
    }
  };
  var ListenerLeakError = class _ListenerLeakError extends Error {
    kind;
    listenerCount;
    /**
     * The detailed message including listener count and most frequent stack.
     * Available locally for debugging but intentionally not used as the error
     * `message`. When `emitterName` is provided, errors group by emitter name
     * and kind in telemetry; otherwise they group by kind alone.
     */
    details;
    constructor(kind, details, stack, listenerCount, emitterName) {
      super(emitterName ? `[${emitterName}] potential listener LEAK detected, ${kind}` : `potential listener LEAK detected, ${kind}`);
      this.name = "ListenerLeakError";
      this.kind = kind;
      this.listenerCount = listenerCount;
      this.details = details;
      this.stack = stack;
    }
    static is(err) {
      return err instanceof _ListenerLeakError || err instanceof Error && typeof err.kind === "string" && typeof err.listenerCount === "number";
    }
  };
  var ListenerRefusalError = class extends ListenerLeakError {
    constructor(kind, details, stack, listenerCount, emitterName) {
      super(kind, details, stack, listenerCount, emitterName);
      this.name = "ListenerRefusalError";
    }
  };
  var id = 0;
  var UniqueContainer = class {
    constructor(value) {
      this.value = value;
    }
    value;
    stack;
    id = id++;
  };
  var compactionThreshold = 2;
  var forEachListener = (listeners, fn) => {
    if (listeners instanceof UniqueContainer) {
      fn(listeners);
    } else {
      for (let i = 0; i < listeners.length; i++) {
        const l = listeners[i];
        if (l) {
          fn(l);
        }
      }
    }
  };
  var Emitter = class {
    _options;
    _leakWarningThreshold;
    _leakWarningName;
    _leakWarningErrorHandler;
    _leakageMon;
    _perfMon;
    _disposed;
    _event;
    /**
     * A listener, or list of listeners. A single listener is the most common
     * for event emitters (#185789), so we optimize that special case to avoid
     * wrapping it in an array (just like Node.js itself.)
     *
     * A list of listeners never 'downgrades' back to a plain function if
     * listeners are removed, for two reasons:
     *
     *  1. That's complicated (especially with the deliveryQueue)
     *  2. A listener with >1 listener is likely to have >1 listener again at
     *     some point, and swapping between arrays and functions may[citation needed]
     *     introduce unnecessary work and garbage.
     *
     * The array listeners can be 'sparse', to avoid reallocating the array
     * whenever any listener is added or removed. If more than `1 / compactionThreshold`
     * of the array is empty, only then is it resized.
     */
    _listeners;
    /**
     * Always to be defined if _listeners is an array. It's no longer a true
     * queue, but holds the dispatching 'state'. If `fire()` is called on an
     * emitter, any work left in the _deliveryQueue is finished first.
     */
    _deliveryQueue;
    _size = 0;
    constructor(options) {
      this._options = options;
      if (_globalLeakWarningThreshold > 0 || this._options?.leakWarningThreshold) {
        this._leakWarningThreshold = this._options?.leakWarningThreshold ?? _globalLeakWarningThreshold;
        this._leakWarningName = this._options?.leakWarningName ?? nextLeakageMonitorName();
        this._leakWarningErrorHandler = this._options?.onListenerError ?? onUnexpectedError;
      }
      this._perfMon = this._options?._profName ? new EventProfiling(this._options._profName) : void 0;
      this._deliveryQueue = this._options?.deliveryQueue;
    }
    _getLeakageMonitor() {
      if (this._leakWarningThreshold === void 0 || this._leakWarningName === void 0 || this._leakWarningErrorHandler === void 0) {
        return void 0;
      }
      return this._leakageMon ??= new LeakageMonitor(this._leakWarningErrorHandler, this._leakWarningThreshold, this._leakWarningName);
    }
    dispose() {
      if (!this._disposed) {
        this._disposed = true;
        if (this._deliveryQueue?.current === this) {
          this._deliveryQueue.reset();
        }
        if (this._listeners) {
          if (_enableDisposeWithListenerWarning) {
            const listeners = this._listeners;
            queueMicrotask(() => {
              forEachListener(listeners, (l) => l.stack?.print());
            });
          }
          this._listeners = void 0;
          this._size = 0;
        }
        this._options?.onDidRemoveLastListener?.();
        this._leakageMon?.dispose();
      }
    }
    /**
     * For the public to allow to subscribe
     * to events from this Emitter
     */
    get event() {
      this._event ??= (callback, thisArgs, disposables) => {
        if (this._leakWarningThreshold !== void 0 && this._size > this._leakWarningThreshold ** 2) {
          const leakageMon = this._getLeakageMonitor();
          if (leakageMon) {
            const message = `[${leakageMon.name}] REFUSES to accept new listeners because it exceeded its threshold by far (${this._size} vs ${leakageMon.threshold})`;
            console.warn(message);
            const tuple = leakageMon.getMostFrequentStack() ?? ["UNKNOWN stack", -1];
            const kind = tuple[1] / this._size > 0.3 ? "dominated" : "popular";
            const error = new ListenerRefusalError(kind, `${message}. HINT: Stack shows most frequent listener (${tuple[1]}-times)`, tuple[0], this._size, this._options?.leakWarningName);
            const errorHandler2 = this._options?.onListenerError || onUnexpectedError;
            errorHandler2(error);
            return Disposable.None;
          }
        }
        if (this._disposed) {
          return Disposable.None;
        }
        if (thisArgs) {
          callback = callback.bind(thisArgs);
        }
        const contained = new UniqueContainer(callback);
        let removeMonitor;
        let stack;
        if (this._leakWarningThreshold !== void 0 && this._size >= Math.ceil(this._leakWarningThreshold * 0.2)) {
          const leakageMon = this._getLeakageMonitor();
          if (leakageMon && this._size + 1 >= leakageMon.threshold) {
            contained.stack = Stacktrace.create();
            removeMonitor = leakageMon.check(contained.stack, this._size + 1);
          }
        }
        if (_enableDisposeWithListenerWarning) {
          contained.stack = stack ?? Stacktrace.create();
        }
        if (!this._listeners) {
          this._options?.onWillAddFirstListener?.(this);
          this._listeners = contained;
          this._options?.onDidAddFirstListener?.(this);
        } else if (this._listeners instanceof UniqueContainer) {
          this._deliveryQueue ??= new EventDeliveryQueuePrivate();
          this._listeners = [this._listeners, contained];
        } else {
          this._listeners.push(contained);
        }
        this._options?.onDidAddListener?.(this);
        this._size++;
        const result = toDisposable(() => {
          removeMonitor?.();
          this._removeListener(contained);
        });
        addToDisposables(result, disposables);
        return result;
      };
      return this._event;
    }
    _removeListener(listener) {
      this._options?.onWillRemoveListener?.(this);
      if (!this._listeners) {
        return;
      }
      if (this._size === 1) {
        this._listeners = void 0;
        this._options?.onDidRemoveLastListener?.(this);
        this._size = 0;
        return;
      }
      const listeners = this._listeners;
      const index = listeners.indexOf(listener);
      if (index === -1) {
        console.log("disposed?", this._disposed);
        console.log("size?", this._size);
        console.log("arr?", JSON.stringify(this._listeners));
        throw new Error("Attempted to dispose unknown listener");
      }
      this._size--;
      listeners[index] = void 0;
      const adjustDeliveryQueue = this._deliveryQueue.current === this;
      if (this._size * compactionThreshold <= listeners.length) {
        let n = 0;
        for (let i = 0; i < listeners.length; i++) {
          if (listeners[i]) {
            listeners[n++] = listeners[i];
          } else if (adjustDeliveryQueue && n < this._deliveryQueue.end) {
            this._deliveryQueue.end--;
            if (n < this._deliveryQueue.i) {
              this._deliveryQueue.i--;
            }
          }
        }
        listeners.length = n;
      }
    }
    _deliver(listener, value) {
      if (!listener) {
        return;
      }
      const errorHandler2 = this._options?.onListenerError || onUnexpectedError;
      if (!errorHandler2) {
        listener.value(value);
        return;
      }
      try {
        listener.value(value);
      } catch (e) {
        errorHandler2(e);
      }
    }
    /** Delivers items in the queue. Assumes the queue is ready to go. */
    _deliverQueue(dq) {
      const listeners = dq.current._listeners;
      while (dq.i < dq.end) {
        this._deliver(listeners[dq.i++], dq.value);
      }
      dq.reset();
    }
    /**
     * To be kept private to fire an event to
     * subscribers
     */
    fire(event) {
      if (this._deliveryQueue?.current) {
        this._deliverQueue(this._deliveryQueue);
        this._perfMon?.stop();
      }
      this._perfMon?.start(this._size);
      if (!this._listeners) {
      } else if (this._listeners instanceof UniqueContainer) {
        this._deliver(this._listeners, event);
      } else {
        const dq = this._deliveryQueue;
        dq.enqueue(this, event, this._listeners.length);
        this._deliverQueue(dq);
      }
      this._perfMon?.stop();
    }
    hasListeners() {
      return this._size > 0;
    }
  };
  var EventDeliveryQueuePrivate = class {
    /**
     * Index in current's listener list.
     */
    i = -1;
    /**
     * The last index in the listener's list to deliver.
     */
    end = 0;
    /**
     * Emitter currently being dispatched on. Emitter._listeners is always an array.
     */
    current;
    /**
     * Currently emitting value. Defined whenever `current` is.
     */
    value;
    enqueue(emitter, value, end) {
      this.i = 0;
      this.end = end;
      this.current = emitter;
      this.value = value;
    }
    reset() {
      this.i = this.end;
      this.current = void 0;
      this.value = void 0;
    }
  };
  function addToDisposables(result, disposables) {
    if (disposables instanceof DisposableStore) {
      disposables.add(result);
    } else if (Array.isArray(disposables)) {
      disposables.push(result);
    }
  }
  function disposeAndRemove(result, disposables) {
    if (disposables instanceof DisposableStore) {
      disposables.delete(result);
    } else if (Array.isArray(disposables)) {
      const index = disposables.indexOf(result);
      if (index !== -1) {
        disposables.splice(index, 1);
      }
    }
    result.dispose();
  }

  // codeoss-m4/stage/ports/codeoss/upstream/vscode-211a00dbedf7a224e0da0a2db3cf9cb6cb7a4bfb/src/vs/base/common/cancellation.ts
  var shortcutEvent = Object.freeze(function(callback, context) {
    const handle = setTimeout(callback.bind(context), 0);
    return { dispose() {
      clearTimeout(handle);
    } };
  });
  var CancellationToken;
  ((CancellationToken3) => {
    function isCancellationToken(thing) {
      if (thing === CancellationToken3.None || thing === CancellationToken3.Cancelled) {
        return true;
      }
      if (thing instanceof MutableToken) {
        return true;
      }
      if (!thing || typeof thing !== "object") {
        return false;
      }
      return typeof thing.isCancellationRequested === "boolean" && typeof thing.onCancellationRequested === "function";
    }
    CancellationToken3.isCancellationToken = isCancellationToken;
    CancellationToken3.None = Object.freeze({
      isCancellationRequested: false,
      onCancellationRequested: Event.None
    });
    CancellationToken3.Cancelled = Object.freeze({
      isCancellationRequested: true,
      onCancellationRequested: shortcutEvent
    });
    function fromAbortSignal(signal, lifetime) {
      if (signal.aborted) {
        return CancellationToken3.Cancelled;
      }
      const token = lifetime.add(new MutableToken());
      lifetime.add(Event.once(Event.fromDOMEventEmitter(signal, "abort"))(() => token.cancel()));
      return token;
    }
    CancellationToken3.fromAbortSignal = fromAbortSignal;
    CancellationToken3.toAbortSignal = (token, lifetime) => {
      if (token === CancellationToken3.None) {
        return new AbortController().signal;
      }
      if (token.isCancellationRequested) {
        return AbortSignal.abort(new CancellationError());
      }
      const controller = new AbortController();
      lifetime.add(token.onCancellationRequested(() => controller.abort(new CancellationError())));
      return controller.signal;
    };
  })(CancellationToken || (CancellationToken = {}));
  var MutableToken = class {
    _isCancelled = false;
    _emitter = null;
    cancel() {
      if (!this._isCancelled) {
        this._isCancelled = true;
        if (this._emitter) {
          this._emitter.fire(void 0);
          this.dispose();
        }
      }
    }
    get isCancellationRequested() {
      return this._isCancelled;
    }
    get onCancellationRequested() {
      if (this._isCancelled) {
        return shortcutEvent;
      }
      if (!this._emitter) {
        this._emitter = new Emitter();
      }
      return this._emitter.event;
    }
    dispose() {
      if (this._emitter) {
        this._emitter.dispose();
        this._emitter = null;
      }
    }
  };

  // codeoss-m4/stage/ports/codeoss/upstream/vscode-211a00dbedf7a224e0da0a2db3cf9cb6cb7a4bfb/src/vs/base/common/cache.ts
  function identity(t) {
    return t;
  }
  var LRUCachedFunction = class {
    lastCache = void 0;
    lastArgKey = void 0;
    _fn;
    _computeKey;
    constructor(arg1, arg2) {
      if (typeof arg1 === "function") {
        this._fn = arg1;
        this._computeKey = identity;
      } else {
        this._fn = arg2;
        this._computeKey = arg1.getCacheKey;
      }
    }
    get(arg) {
      const key = this._computeKey(arg);
      if (this.lastArgKey !== key) {
        this.lastArgKey = key;
        this.lastCache = this._fn(arg);
      }
      return this.lastCache;
    }
  };

  // codeoss-m4/stage/ports/codeoss/upstream/vscode-211a00dbedf7a224e0da0a2db3cf9cb6cb7a4bfb/src/vs/base/common/lazy.ts
  var Lazy = class {
    constructor(executor) {
      this.executor = executor;
    }
    executor;
    _state = 0 /* Uninitialized */;
    _value;
    _error;
    /**
     * True if the lazy value has been resolved.
     */
    get hasValue() {
      return this._state === 2 /* Completed */;
    }
    /**
     * Get the wrapped value.
     *
     * This will force evaluation of the lazy value if it has not been resolved yet. Lazy values are only
     * resolved once. `getValue` will re-throw exceptions that are hit while resolving the value
     */
    get value() {
      if (this._state === 0 /* Uninitialized */) {
        this._state = 1 /* Running */;
        try {
          this._value = this.executor();
        } catch (err) {
          this._error = err;
        } finally {
          this._state = 2 /* Completed */;
        }
      } else if (this._state === 1 /* Running */) {
        throw new Error("Cannot read the value of a lazy that is being initialized");
      }
      if (this._error) {
        throw this._error;
      }
      return this._value;
    }
    /**
     * Get the wrapped value without forcing evaluation.
     */
    get rawValue() {
      return this._value;
    }
  };

  // codeoss-m4/stage/ports/codeoss/upstream/vscode-211a00dbedf7a224e0da0a2db3cf9cb6cb7a4bfb/src/vs/base/common/strings.ts
  function isHighSurrogate(charCode) {
    return 55296 <= charCode && charCode <= 56319;
  }
  function isLowSurrogate(charCode) {
    return 56320 <= charCode && charCode <= 57343;
  }
  function computeCodePoint(highSurrogate, lowSurrogate) {
    return (highSurrogate - 55296 << 10) + (lowSurrogate - 56320) + 65536;
  }
  function getNextCodePoint(str, len, offset) {
    const charCode = str.charCodeAt(offset);
    if (isHighSurrogate(charCode) && offset + 1 < len) {
      const nextCharCode = str.charCodeAt(offset + 1);
      if (isLowSurrogate(nextCharCode)) {
        return computeCodePoint(charCode, nextCharCode);
      }
    }
    return charCode;
  }
  var CSI_SEQUENCE = /(?:\x1b\[|\x9b)[=?>!]?[\d;:]*["$#'* ]?[a-zA-Z@^`{}|~]/;
  var OSC_SEQUENCE = /(?:\x1b\]|\x9d).*?(?:\x1b\\|\x07|\x9c)/;
  var ESC_SEQUENCE = /\x1b(?:[ #%\(\)\*\+\-\.\/]?[a-zA-Z0-9\|}~@])/;
  var CONTROL_SEQUENCES = new RegExp("(?:" + [
    CSI_SEQUENCE.source,
    OSC_SEQUENCE.source,
    ESC_SEQUENCE.source
  ].join("|") + ")", "g");
  var UTF8_BOM_CHARACTER = String.fromCharCode(65279 /* UTF8_BOM */);
  var AmbiguousCharacters = class _AmbiguousCharacters {
    constructor(confusableDictionary) {
      this.confusableDictionary = confusableDictionary;
    }
    confusableDictionary;
    static ambiguousCharacterData = new Lazy(() => {
      return JSON.parse(
        '{"_common":[8232,32,8233,32,5760,32,8192,32,8193,32,8194,32,8195,32,8196,32,8197,32,8198,32,8200,32,8201,32,8202,32,8287,32,8199,32,8239,32,2042,95,65101,95,65102,95,65103,95,8208,45,8209,45,8210,45,65112,45,1748,45,8259,45,727,45,8722,45,10134,45,11450,45,1549,44,1643,44,184,44,42233,44,894,59,2307,58,2691,58,1417,58,1795,58,1796,58,5868,58,65072,58,6147,58,6153,58,8282,58,1475,58,760,58,42889,58,8758,58,720,58,42237,58,451,33,11601,33,660,63,577,63,2429,63,5038,63,42731,63,119149,46,8228,46,1793,46,1794,46,42510,46,68176,46,1632,46,1776,46,42232,46,1373,96,65287,96,8219,96,1523,96,8242,96,1370,96,8175,96,65344,96,900,96,8189,96,8125,96,8127,96,8190,96,697,96,884,96,712,96,714,96,715,96,756,96,699,96,701,96,700,96,702,96,42892,96,1497,96,2036,96,2037,96,5194,96,5836,96,94033,96,94034,96,65339,91,10088,40,10098,40,12308,40,64830,40,65341,93,10089,41,10099,41,12309,41,64831,41,10100,123,119060,123,10101,125,65342,94,8270,42,1645,42,8727,42,66335,42,5941,47,8257,47,8725,47,8260,47,9585,47,10187,47,10744,47,119354,47,12755,47,12339,47,11462,47,20031,47,12035,47,65340,92,65128,92,8726,92,10189,92,10741,92,10745,92,119311,92,119355,92,12756,92,20022,92,12034,92,42872,38,708,94,710,94,5869,43,10133,43,66203,43,8249,60,10094,60,706,60,119350,60,5176,60,5810,60,5120,61,11840,61,12448,61,42239,61,8250,62,10095,62,707,62,119351,62,5171,62,94015,62,8275,126,732,126,8128,126,8764,126,65372,124,65293,45,118002,50,120784,50,120794,50,120804,50,120814,50,120824,50,130034,50,42842,50,423,50,1000,50,42564,50,5311,50,42735,50,119302,51,118003,51,120785,51,120795,51,120805,51,120815,51,120825,51,130035,51,42923,51,540,51,439,51,42858,51,11468,51,1248,51,94011,51,71882,51,118004,52,120786,52,120796,52,120806,52,120816,52,120826,52,130036,52,5070,52,71855,52,118005,53,120787,53,120797,53,120807,53,120817,53,120827,53,130037,53,444,53,71867,53,118006,54,120788,54,120798,54,120808,54,120818,54,120828,54,130038,54,11474,54,5102,54,71893,54,119314,55,118007,55,120789,55,120799,55,120809,55,120819,55,120829,55,130039,55,66770,55,71878,55,2819,56,2538,56,2666,56,125131,56,118008,56,120790,56,120800,56,120810,56,120820,56,120830,56,130040,56,547,56,546,56,66330,56,2663,57,2920,57,2541,57,3437,57,118009,57,120791,57,120801,57,120811,57,120821,57,120831,57,130041,57,42862,57,11466,57,71884,57,71852,57,71894,57,9082,97,65345,97,119834,97,119886,97,119938,97,119990,97,120042,97,120094,97,120146,97,120198,97,120250,97,120302,97,120354,97,120406,97,120458,97,593,97,945,97,120514,97,120572,97,120630,97,120688,97,120746,97,65313,65,117974,65,119808,65,119860,65,119912,65,119964,65,120016,65,120068,65,120120,65,120172,65,120224,65,120276,65,120328,65,120380,65,120432,65,913,65,120488,65,120546,65,120604,65,120662,65,120720,65,5034,65,5573,65,42222,65,94016,65,66208,65,119835,98,119887,98,119939,98,119991,98,120043,98,120095,98,120147,98,120199,98,120251,98,120303,98,120355,98,120407,98,120459,98,388,98,5071,98,5234,98,5551,98,65314,66,8492,66,117975,66,119809,66,119861,66,119913,66,120017,66,120069,66,120121,66,120173,66,120225,66,120277,66,120329,66,120381,66,120433,66,42932,66,914,66,120489,66,120547,66,120605,66,120663,66,120721,66,5108,66,5623,66,42192,66,66178,66,66209,66,66305,66,65347,99,8573,99,119836,99,119888,99,119940,99,119992,99,120044,99,120096,99,120148,99,120200,99,120252,99,120304,99,120356,99,120408,99,120460,99,7428,99,1010,99,11429,99,43951,99,66621,99,128844,67,71913,67,71922,67,65315,67,8557,67,8450,67,8493,67,117976,67,119810,67,119862,67,119914,67,119966,67,120018,67,120174,67,120226,67,120278,67,120330,67,120382,67,120434,67,1017,67,11428,67,5087,67,42202,67,66210,67,66306,67,66581,67,66844,67,8574,100,8518,100,119837,100,119889,100,119941,100,119993,100,120045,100,120097,100,120149,100,120201,100,120253,100,120305,100,120357,100,120409,100,120461,100,1281,100,5095,100,5231,100,42194,100,8558,68,8517,68,117977,68,119811,68,119863,68,119915,68,119967,68,120019,68,120071,68,120123,68,120175,68,120227,68,120279,68,120331,68,120383,68,120435,68,5024,68,5598,68,5610,68,42195,68,8494,101,65349,101,8495,101,8519,101,119838,101,119890,101,119942,101,120046,101,120098,101,120150,101,120202,101,120254,101,120306,101,120358,101,120410,101,120462,101,43826,101,1213,101,8959,69,65317,69,8496,69,117978,69,119812,69,119864,69,119916,69,120020,69,120072,69,120124,69,120176,69,120228,69,120280,69,120332,69,120384,69,120436,69,917,69,120492,69,120550,69,120608,69,120666,69,120724,69,11577,69,5036,69,42224,69,71846,69,71854,69,66182,69,119839,102,119891,102,119943,102,119995,102,120047,102,120099,102,120151,102,120203,102,120255,102,120307,102,120359,102,120411,102,120463,102,43829,102,42905,102,383,102,7837,102,1412,102,119315,70,8497,70,117979,70,119813,70,119865,70,119917,70,120021,70,120073,70,120125,70,120177,70,120229,70,120281,70,120333,70,120385,70,120437,70,42904,70,988,70,120778,70,5556,70,42205,70,71874,70,71842,70,66183,70,66213,70,66853,70,65351,103,8458,103,119840,103,119892,103,119944,103,120048,103,120100,103,120152,103,120204,103,120256,103,120308,103,120360,103,120412,103,120464,103,609,103,7555,103,397,103,1409,103,117980,71,119814,71,119866,71,119918,71,119970,71,120022,71,120074,71,120126,71,120178,71,120230,71,120282,71,120334,71,120386,71,120438,71,1292,71,5056,71,5107,71,42198,71,65352,104,8462,104,119841,104,119945,104,119997,104,120049,104,120101,104,120153,104,120205,104,120257,104,120309,104,120361,104,120413,104,120465,104,1211,104,1392,104,5058,104,65320,72,8459,72,8460,72,8461,72,117981,72,119815,72,119867,72,119919,72,120023,72,120179,72,120231,72,120283,72,120335,72,120387,72,120439,72,919,72,120494,72,120552,72,120610,72,120668,72,120726,72,11406,72,5051,72,5500,72,42215,72,66255,72,731,105,9075,105,65353,105,8560,105,8505,105,8520,105,119842,105,119894,105,119946,105,119998,105,120050,105,120102,105,120154,105,120206,105,120258,105,120310,105,120362,105,120414,105,120466,105,120484,105,618,105,617,105,953,105,8126,105,890,105,120522,105,120580,105,120638,105,120696,105,120754,105,1110,105,42567,105,1231,105,43893,105,5029,105,71875,105,65354,106,8521,106,119843,106,119895,106,119947,106,119999,106,120051,106,120103,106,120155,106,120207,106,120259,106,120311,106,120363,106,120415,106,120467,106,1011,106,1112,106,65322,74,117983,74,119817,74,119869,74,119921,74,119973,74,120025,74,120077,74,120129,74,120181,74,120233,74,120285,74,120337,74,120389,74,120441,74,42930,74,895,74,1032,74,5035,74,5261,74,42201,74,119844,107,119896,107,119948,107,120000,107,120052,107,120104,107,120156,107,120208,107,120260,107,120312,107,120364,107,120416,107,120468,107,8490,75,65323,75,117984,75,119818,75,119870,75,119922,75,119974,75,120026,75,120078,75,120130,75,120182,75,120234,75,120286,75,120338,75,120390,75,120442,75,922,75,120497,75,120555,75,120613,75,120671,75,120729,75,11412,75,5094,75,5845,75,42199,75,66840,75,1472,108,8739,73,9213,73,65512,73,1633,108,1777,73,66336,108,125127,108,118001,108,120783,73,120793,73,120803,73,120813,73,120823,73,130033,73,65321,73,8544,73,8464,73,8465,73,117982,108,119816,73,119868,73,119920,73,120024,73,120128,73,120180,73,120232,73,120284,73,120336,73,120388,73,120440,73,65356,108,8572,73,8467,108,119845,108,119897,108,119949,108,120001,108,120053,108,120105,73,120157,73,120209,73,120261,73,120313,73,120365,73,120417,73,120469,73,448,73,120496,73,120554,73,120612,73,120670,73,120728,73,11410,73,1030,73,1216,73,1493,108,1503,108,1575,108,126464,108,126592,108,65166,108,65165,108,1994,108,11599,73,5825,73,42226,73,93992,73,66186,124,66313,124,119338,76,8556,76,8466,76,117985,76,119819,76,119871,76,119923,76,120027,76,120079,76,120131,76,120183,76,120235,76,120287,76,120339,76,120391,76,120443,76,11472,76,5086,76,5290,76,42209,76,93974,76,71843,76,71858,76,66587,76,66854,76,65325,77,8559,77,8499,77,117986,77,119820,77,119872,77,119924,77,120028,77,120080,77,120132,77,120184,77,120236,77,120288,77,120340,77,120392,77,120444,77,924,77,120499,77,120557,77,120615,77,120673,77,120731,77,1018,77,11416,77,5047,77,5616,77,5846,77,42207,77,66224,77,66321,77,119847,110,119899,110,119951,110,120003,110,120055,110,120107,110,120159,110,120211,110,120263,110,120315,110,120367,110,120419,110,120471,110,1400,110,1404,110,65326,78,8469,78,117987,78,119821,78,119873,78,119925,78,119977,78,120029,78,120081,78,120185,78,120237,78,120289,78,120341,78,120393,78,120445,78,925,78,120500,78,120558,78,120616,78,120674,78,120732,78,11418,78,42208,78,66835,78,3074,111,3202,111,3330,111,3458,111,2406,111,2662,111,2790,111,3046,111,3174,111,3302,111,3430,111,3664,111,3792,111,4160,111,1637,111,1781,111,65359,111,8500,111,119848,111,119900,111,119952,111,120056,111,120108,111,120160,111,120212,111,120264,111,120316,111,120368,111,120420,111,120472,111,7439,111,7441,111,43837,111,959,111,120528,111,120586,111,120644,111,120702,111,120760,111,963,111,120532,111,120590,111,120648,111,120706,111,120764,111,11423,111,4351,111,1413,111,1505,111,1607,111,126500,111,126564,111,126596,111,65259,111,65260,111,65258,111,65257,111,1726,111,64428,111,64429,111,64427,111,64426,111,1729,111,64424,111,64425,111,64423,111,64422,111,1749,111,3360,111,4125,111,66794,111,71880,111,71895,111,66604,111,1984,79,2534,79,2918,79,12295,79,70864,79,71904,79,118000,79,120782,79,120792,79,120802,79,120812,79,120822,79,130032,79,65327,79,117988,79,119822,79,119874,79,119926,79,119978,79,120030,79,120082,79,120134,79,120186,79,120238,79,120290,79,120342,79,120394,79,120446,79,927,79,120502,79,120560,79,120618,79,120676,79,120734,79,11422,79,1365,79,11604,79,4816,79,2848,79,66754,79,42227,79,71861,79,66194,79,66219,79,66564,79,66838,79,9076,112,65360,112,119849,112,119901,112,119953,112,120005,112,120057,112,120109,112,120161,112,120213,112,120265,112,120317,112,120369,112,120421,112,120473,112,961,112,120530,112,120544,112,120588,112,120602,112,120646,112,120660,112,120704,112,120718,112,120762,112,120776,112,11427,112,65328,80,8473,80,117989,80,119823,80,119875,80,119927,80,119979,80,120031,80,120083,80,120187,80,120239,80,120291,80,120343,80,120395,80,120447,80,929,80,120504,80,120562,80,120620,80,120678,80,120736,80,11426,80,5090,80,5229,80,42193,80,66197,80,119850,113,119902,113,119954,113,120006,113,120058,113,120110,113,120162,113,120214,113,120266,113,120318,113,120370,113,120422,113,120474,113,1307,113,1379,113,1382,113,8474,81,117990,81,119824,81,119876,81,119928,81,119980,81,120032,81,120084,81,120188,81,120240,81,120292,81,120344,81,120396,81,120448,81,11605,81,119851,114,119903,114,119955,114,120007,114,120059,114,120111,114,120163,114,120215,114,120267,114,120319,114,120371,114,120423,114,120475,114,43847,114,43848,114,7462,114,11397,114,43905,114,119318,82,8475,82,8476,82,8477,82,117991,82,119825,82,119877,82,119929,82,120033,82,120189,82,120241,82,120293,82,120345,82,120397,82,120449,82,422,82,5025,82,5074,82,66740,82,5511,82,42211,82,94005,82,65363,115,119852,115,119904,115,119956,115,120008,115,120060,115,120112,115,120164,115,120216,115,120268,115,120320,115,120372,115,120424,115,120476,115,42801,115,445,115,1109,115,43946,115,71873,115,66632,115,65331,83,117992,83,119826,83,119878,83,119930,83,119982,83,120034,83,120086,83,120138,83,120190,83,120242,83,120294,83,120346,83,120398,83,120450,83,1029,83,1359,83,5077,83,5082,83,42210,83,94010,83,66198,83,66592,83,119853,116,119905,116,119957,116,120009,116,120061,116,120113,116,120165,116,120217,116,120269,116,120321,116,120373,116,120425,116,120477,116,8868,84,10201,84,128872,84,65332,84,117993,84,119827,84,119879,84,119931,84,119983,84,120035,84,120087,84,120139,84,120191,84,120243,84,120295,84,120347,84,120399,84,120451,84,932,84,120507,84,120565,84,120623,84,120681,84,120739,84,11430,84,5026,84,42196,84,93962,84,71868,84,66199,84,66225,84,66325,84,119854,117,119906,117,119958,117,120010,117,120062,117,120114,117,120166,117,120218,117,120270,117,120322,117,120374,117,120426,117,120478,117,42911,117,7452,117,43854,117,43858,117,651,117,965,117,120534,117,120592,117,120650,117,120708,117,120766,117,1405,117,66806,117,71896,117,8746,85,8899,85,117994,85,119828,85,119880,85,119932,85,119984,85,120036,85,120088,85,120140,85,120192,85,120244,85,120296,85,120348,85,120400,85,120452,85,1357,85,4608,85,66766,85,5196,85,42228,85,94018,85,71864,85,8744,118,8897,118,65366,118,8564,118,119855,118,119907,118,119959,118,120011,118,120063,118,120115,118,120167,118,120219,118,120271,118,120323,118,120375,118,120427,118,120479,118,7456,118,957,118,120526,118,120584,118,120642,118,120700,118,120758,118,1141,118,1496,118,71430,118,43945,118,71872,118,119309,86,1639,86,1783,86,8548,86,117995,86,119829,86,119881,86,119933,86,119985,86,120037,86,120089,86,120141,86,120193,86,120245,86,120297,86,120349,86,120401,86,120453,86,1140,86,11576,86,5081,86,5167,86,42719,86,42214,86,93960,86,71840,86,66845,86,623,119,119856,119,119908,119,119960,119,120012,119,120064,119,120116,119,120168,119,120220,119,120272,119,120324,119,120376,119,120428,119,120480,119,7457,119,1121,119,1309,119,1377,119,71434,119,71438,119,71439,119,43907,119,71910,87,71919,87,117996,87,119830,87,119882,87,119934,87,119986,87,120038,87,120090,87,120142,87,120194,87,120246,87,120298,87,120350,87,120402,87,120454,87,1308,87,5043,87,5076,87,42218,87,5742,120,10539,120,10540,120,10799,120,65368,120,8569,120,119857,120,119909,120,119961,120,120013,120,120065,120,120117,120,120169,120,120221,120,120273,120,120325,120,120377,120,120429,120,120481,120,5441,120,5501,120,5741,88,9587,88,66338,88,71916,88,65336,88,8553,88,117997,88,119831,88,119883,88,119935,88,119987,88,120039,88,120091,88,120143,88,120195,88,120247,88,120299,88,120351,88,120403,88,120455,88,42931,88,935,88,120510,88,120568,88,120626,88,120684,88,120742,88,11436,88,11613,88,5815,88,42219,88,66192,88,66228,88,66327,88,66855,88,611,121,7564,121,65369,121,119858,121,119910,121,119962,121,120014,121,120066,121,120118,121,120170,121,120222,121,120274,121,120326,121,120378,121,120430,121,120482,121,655,121,7935,121,43866,121,947,121,8509,121,120516,121,120574,121,120632,121,120690,121,120748,121,1199,121,4327,121,71900,121,65337,89,117998,89,119832,89,119884,89,119936,89,119988,89,120040,89,120092,89,120144,89,120196,89,120248,89,120300,89,120352,89,120404,89,120456,89,933,89,978,89,120508,89,120566,89,120624,89,120682,89,120740,89,11432,89,1198,89,5033,89,5053,89,42220,89,94019,89,71844,89,66226,89,119859,122,119911,122,119963,122,120015,122,120067,122,120119,122,120171,122,120223,122,120275,122,120327,122,120379,122,120431,122,120483,122,7458,122,43923,122,71876,122,71909,90,66293,90,65338,90,8484,90,8488,90,117999,90,119833,90,119885,90,119937,90,119989,90,120041,90,120197,90,120249,90,120301,90,120353,90,120405,90,120457,90,918,90,120493,90,120551,90,120609,90,120667,90,120725,90,5059,90,42204,90,71849,90,65282,34,65283,35,65284,36,65285,37,65286,38,65290,42,65291,43,65294,46,65295,47,65296,48,65298,50,65299,51,65300,52,65301,53,65302,54,65303,55,65304,56,65305,57,65308,60,65309,61,65310,62,65312,64,65316,68,65318,70,65319,71,65324,76,65329,81,65330,82,65333,85,65334,86,65335,87,65343,95,65346,98,65348,100,65350,102,65355,107,65357,109,65358,110,65361,113,65362,114,65364,116,65365,117,65367,119,65370,122,65371,123,65373,125,119846,109],"_default":[160,32,8211,45,65374,126,8218,44,65306,58,65281,33,8216,96,8217,96,8245,96,180,96,12494,47,1047,51,1073,54,1072,97,1040,65,1068,98,1042,66,1089,99,1057,67,1077,101,1045,69,1053,72,305,105,1050,75,921,73,1052,77,1086,111,1054,79,1009,112,1088,112,1056,80,1075,114,1058,84,215,120,1093,120,1061,88,1091,121,1059,89,65288,40,65289,41,65292,44,65297,49,65307,59,65311,63],"cs":[65374,126,8218,44,65306,58,65281,33,8216,96,8245,96,180,96,12494,47,1047,51,1073,54,1072,97,1040,65,1068,98,1042,66,1089,99,1057,67,1077,101,1045,69,1053,72,305,105,1050,75,921,73,1052,77,1086,111,1054,79,1009,112,1088,112,1056,80,1075,114,1058,84,1093,120,1061,88,1091,121,1059,89,65288,40,65289,41,65292,44,65297,49,65307,59,65311,63],"de":[65374,126,65306,58,65281,33,8245,96,180,96,12494,47,1047,51,1073,54,1072,97,1040,65,1068,98,1042,66,1089,99,1057,67,1077,101,1045,69,1053,72,305,105,1050,75,921,73,1052,77,1086,111,1054,79,1009,112,1088,112,1056,80,1075,114,1058,84,1093,120,1061,88,1091,121,1059,89,65288,40,65289,41,65292,44,65297,49,65307,59,65311,63],"es":[8211,45,65374,126,8218,44,65306,58,65281,33,8245,96,180,96,12494,47,1047,51,1073,54,1072,97,1040,65,1068,98,1042,66,1089,99,1057,67,1077,101,1045,69,1053,72,305,105,1050,75,1052,77,1086,111,1054,79,1009,112,1088,112,1056,80,1075,114,1058,84,215,120,1093,120,1061,88,1091,121,1059,89,65288,40,65289,41,65292,44,65297,49,65307,59,65311,63],"fr":[65374,126,8218,44,65306,58,65281,33,8216,96,8245,96,12494,47,1047,51,1073,54,1072,97,1040,65,1068,98,1042,66,1089,99,1057,67,1077,101,1045,69,1053,72,305,105,1050,75,921,73,1052,77,1086,111,1054,79,1009,112,1088,112,1056,80,1075,114,1058,84,215,120,1093,120,1061,88,1091,121,1059,89,65288,40,65289,41,65292,44,65297,49,65307,59,65311,63],"it":[160,32,8211,45,65374,126,8218,44,65306,58,65281,33,8245,96,180,96,12494,47,1047,51,1073,54,1072,97,1040,65,1068,98,1042,66,1089,99,1057,67,1077,101,1045,69,1053,72,305,105,1050,75,921,73,1052,77,1086,111,1054,79,1009,112,1088,112,1056,80,1075,114,1058,84,215,120,1093,120,1061,88,1091,121,1059,89,65288,40,65289,41,65292,44,65297,49,65307,59,65311,63],"ja":[8211,45,8218,44,65281,33,8216,96,8245,96,180,96,1047,51,1073,54,1072,97,1040,65,1068,98,1042,66,1089,99,1057,67,1077,101,1045,69,1053,72,305,105,1050,75,921,73,1052,77,1086,111,1054,79,1009,112,1088,112,1056,80,1075,114,1058,84,215,120,1093,120,1061,88,1091,121,1059,89,65292,44,65297,49,65307,59],"ko":[8211,45,65374,126,8218,44,65306,58,65281,33,8245,96,180,96,12494,47,1047,51,1073,54,1072,97,1040,65,1068,98,1042,66,1089,99,1057,67,1077,101,1045,69,1053,72,305,105,1050,75,921,73,1052,77,1086,111,1054,79,1009,112,1088,112,1056,80,1075,114,1058,84,215,120,1093,120,1061,88,1091,121,1059,89,65288,40,65289,41,65292,44,65297,49,65307,59,65311,63],"pl":[65374,126,65306,58,65281,33,8216,96,8245,96,180,96,12494,47,1047,51,1073,54,1072,97,1040,65,1068,98,1042,66,1089,99,1057,67,1077,101,1045,69,1053,72,305,105,1050,75,921,73,1052,77,1086,111,1054,79,1009,112,1088,112,1056,80,1075,114,1058,84,215,120,1093,120,1061,88,1091,121,1059,89,65288,40,65289,41,65292,44,65297,49,65307,59,65311,63],"pt-BR":[65374,126,8218,44,65306,58,65281,33,8216,96,8245,96,180,96,12494,47,1047,51,1073,54,1072,97,1040,65,1068,98,1042,66,1089,99,1057,67,1077,101,1045,69,1053,72,305,105,1050,75,921,73,1052,77,1086,111,1054,79,1009,112,1088,112,1056,80,1075,114,1058,84,215,120,1093,120,1061,88,1091,121,1059,89,65288,40,65289,41,65292,44,65297,49,65307,59,65311,63],"qps-ploc":[160,32,8211,45,65374,126,8218,44,65306,58,65281,33,8216,96,8245,96,180,96,12494,47,1047,51,1073,54,1072,97,1040,65,1068,98,1042,66,1089,99,1057,67,1077,101,1045,69,1053,72,305,105,1050,75,921,73,1052,77,1086,111,1054,79,1088,112,1056,80,1075,114,1058,84,215,120,1093,120,1061,88,1091,121,1059,89,65288,40,65289,41,65292,44,65297,49,65307,59,65311,63],"ru":[65374,126,8218,44,65306,58,65281,33,8216,96,8245,96,180,96,12494,47,305,105,921,73,1009,112,215,120,65288,40,65289,41,65292,44,65297,49,65307,59,65311,63],"tr":[160,32,8211,45,65374,126,8218,44,65306,58,65281,33,8245,96,180,96,12494,47,1047,51,1073,54,1072,97,1040,65,1068,98,1042,66,1089,99,1057,67,1077,101,1045,69,1053,72,1050,75,921,73,1052,77,1086,111,1054,79,1009,112,1088,112,1056,80,1075,114,1058,84,215,120,1093,120,1061,88,1091,121,1059,89,65288,40,65289,41,65292,44,65297,49,65307,59,65311,63],"zh-hans":[160,32,65374,126,8218,44,8245,96,180,96,12494,47,1047,51,1073,54,1072,97,1040,65,1068,98,1042,66,1089,99,1057,67,1077,101,1045,69,1053,72,305,105,1050,75,921,73,1052,77,1086,111,1054,79,1009,112,1088,112,1056,80,1075,114,1058,84,215,120,1093,120,1061,88,1091,121,1059,89,65297,49],"zh-hant":[8211,45,65374,126,8218,44,180,96,12494,47,1047,51,1073,54,1072,97,1040,65,1068,98,1042,66,1089,99,1057,67,1077,101,1045,69,1053,72,305,105,1050,75,921,73,1052,77,1086,111,1054,79,1009,112,1088,112,1056,80,1075,114,1058,84,215,120,1093,120,1061,88,1091,121,1059,89]}'
      );
    });
    static cache = new LRUCachedFunction((localesStr) => {
      const locales = localesStr.split(",");
      function arrayToMap(arr) {
        const result = /* @__PURE__ */ new Map();
        for (let i = 0; i < arr.length; i += 2) {
          result.set(arr[i], arr[i + 1]);
        }
        return result;
      }
      function mergeMaps(map1, map2) {
        const result = new Map(map1);
        for (const [key, value] of map2) {
          result.set(key, value);
        }
        return result;
      }
      function intersectMaps(map1, map2) {
        if (!map1) {
          return map2;
        }
        const result = /* @__PURE__ */ new Map();
        for (const [key, value] of map1) {
          if (map2.has(key)) {
            result.set(key, value);
          }
        }
        return result;
      }
      const data = this.ambiguousCharacterData.value;
      let filteredLocales = locales.filter(
        (l) => !l.startsWith("_") && Object.hasOwn(data, l)
      );
      if (filteredLocales.length === 0) {
        filteredLocales = ["_default"];
      }
      let languageSpecificMap = void 0;
      for (const locale of filteredLocales) {
        const map2 = arrayToMap(data[locale]);
        languageSpecificMap = intersectMaps(languageSpecificMap, map2);
      }
      const commonMap = arrayToMap(data["_common"]);
      const map = mergeMaps(commonMap, languageSpecificMap);
      return new _AmbiguousCharacters(map);
    });
    static getInstance(locales) {
      return _AmbiguousCharacters.cache.get(Array.from(locales).join(","));
    }
    static _locales = new Lazy(
      () => Object.keys(_AmbiguousCharacters.ambiguousCharacterData.value).filter(
        (k) => !k.startsWith("_")
      )
    );
    static getLocales() {
      return _AmbiguousCharacters._locales.value;
    }
    isAmbiguous(codePoint) {
      return this.confusableDictionary.has(codePoint);
    }
    containsAmbiguousCharacter(str) {
      for (let i = 0; i < str.length; i++) {
        const codePoint = str.codePointAt(i);
        if (typeof codePoint === "number" && this.isAmbiguous(codePoint)) {
          return true;
        }
      }
      return false;
    }
    /**
     * Returns the non basic ASCII code point that the given code point can be confused,
     * or undefined if such code point does note exist.
     */
    getPrimaryConfusable(codePoint) {
      return this.confusableDictionary.get(codePoint);
    }
    getConfusableCodePoints() {
      return new Set(this.confusableDictionary.keys());
    }
  };

  // codeoss-m4/stage/ports/codeoss/upstream/vscode-211a00dbedf7a224e0da0a2db3cf9cb6cb7a4bfb/src/vs/base/common/date.ts
  var minute = 60;
  var hour = minute * 60;
  var day = hour * 24;
  var week = day * 7;
  var month = day * 30;
  var year = day * 365;

  // codeoss-m4/stage/ports/codeoss/upstream/vscode-211a00dbedf7a224e0da0a2db3cf9cb6cb7a4bfb/src/vs/editor/common/core/wordCharacterClassifier.ts
  var wordClassifierCache = new LRUCache(10);

  // codeoss-m4/stage/ports/codeoss/upstream/vscode-211a00dbedf7a224e0da0a2db3cf9cb6cb7a4bfb/src/vs/editor/common/model/textModelSearch.ts
  function createFindMatch(range, rawMatches, captureMatches) {
    if (!captureMatches) {
      return new FindMatch(range, null);
    }
    const matches = [];
    for (let i = 0, len = rawMatches.length; i < len; i++) {
      matches[i] = rawMatches[i];
    }
    return new FindMatch(range, matches);
  }
  function leftIsWordBounday(wordSeparators, text, textLength, matchStartIndex, matchLength) {
    if (matchStartIndex === 0) {
      return true;
    }
    const charBefore = text.charCodeAt(matchStartIndex - 1);
    if (wordSeparators.get(charBefore) !== 0 /* Regular */) {
      return true;
    }
    if (charBefore === 13 /* CarriageReturn */ || charBefore === 10 /* LineFeed */) {
      return true;
    }
    if (matchLength > 0) {
      const firstCharInMatch = text.charCodeAt(matchStartIndex);
      if (wordSeparators.get(firstCharInMatch) !== 0 /* Regular */) {
        return true;
      }
    }
    return false;
  }
  function rightIsWordBounday(wordSeparators, text, textLength, matchStartIndex, matchLength) {
    if (matchStartIndex + matchLength === textLength) {
      return true;
    }
    const charAfter = text.charCodeAt(matchStartIndex + matchLength);
    if (wordSeparators.get(charAfter) !== 0 /* Regular */) {
      return true;
    }
    if (charAfter === 13 /* CarriageReturn */ || charAfter === 10 /* LineFeed */) {
      return true;
    }
    if (matchLength > 0) {
      const lastCharInMatch = text.charCodeAt(matchStartIndex + matchLength - 1);
      if (wordSeparators.get(lastCharInMatch) !== 0 /* Regular */) {
        return true;
      }
    }
    return false;
  }
  function isValidMatch(wordSeparators, text, textLength, matchStartIndex, matchLength) {
    return leftIsWordBounday(wordSeparators, text, textLength, matchStartIndex, matchLength) && rightIsWordBounday(wordSeparators, text, textLength, matchStartIndex, matchLength);
  }
  var Searcher = class {
    _wordSeparators;
    _searchRegex;
    _prevMatchStartIndex;
    _prevMatchLength;
    constructor(wordSeparators, searchRegex) {
      this._wordSeparators = wordSeparators;
      this._searchRegex = searchRegex;
      this._prevMatchStartIndex = -1;
      this._prevMatchLength = 0;
    }
    reset(lastIndex) {
      this._searchRegex.lastIndex = lastIndex;
      this._prevMatchStartIndex = -1;
      this._prevMatchLength = 0;
    }
    next(text) {
      const textLength = text.length;
      let m;
      do {
        if (this._prevMatchStartIndex + this._prevMatchLength === textLength) {
          return null;
        }
        m = this._searchRegex.exec(text);
        if (!m) {
          return null;
        }
        const matchStartIndex = m.index;
        const matchLength = m[0].length;
        if (matchStartIndex === this._prevMatchStartIndex && matchLength === this._prevMatchLength) {
          if (matchLength === 0) {
            if (getNextCodePoint(text, textLength, this._searchRegex.lastIndex) > 65535) {
              this._searchRegex.lastIndex += 2;
            } else {
              this._searchRegex.lastIndex += 1;
            }
            continue;
          }
          return null;
        }
        this._prevMatchStartIndex = matchStartIndex;
        this._prevMatchLength = matchLength;
        if (!this._wordSeparators || isValidMatch(this._wordSeparators, text, textLength, matchStartIndex, matchLength)) {
          return m;
        }
      } while (m);
      return null;
    }
  };

  // codeoss-m4/stage/ports/codeoss/upstream/vscode-211a00dbedf7a224e0da0a2db3cf9cb6cb7a4bfb/src/vs/editor/common/model/pieceTreeTextBuffer/pieceTreeBase.ts
  var AverageBufferSize = 65535;
  function createUintArray(arr) {
    let r;
    if (arr[arr.length - 1] < 65536) {
      r = new Uint16Array(arr.length);
    } else {
      r = new Uint32Array(arr.length);
    }
    r.set(arr, 0);
    return r;
  }
  function createLineStartsFast(str, readonly = true) {
    const r = [0];
    let rLength = 1;
    for (let i = 0, len = str.length; i < len; i++) {
      const chr = str.charCodeAt(i);
      if (chr === 13 /* CarriageReturn */) {
        if (i + 1 < len && str.charCodeAt(i + 1) === 10 /* LineFeed */) {
          r[rLength++] = i + 2;
          i++;
        } else {
          r[rLength++] = i + 1;
        }
      } else if (chr === 10 /* LineFeed */) {
        r[rLength++] = i + 1;
      }
    }
    if (readonly) {
      return createUintArray(r);
    } else {
      return r;
    }
  }
  var Piece = class {
    bufferIndex;
    start;
    end;
    length;
    lineFeedCnt;
    constructor(bufferIndex, start, end, lineFeedCnt, length) {
      this.bufferIndex = bufferIndex;
      this.start = start;
      this.end = end;
      this.lineFeedCnt = lineFeedCnt;
      this.length = length;
    }
  };
  var StringBuffer = class {
    buffer;
    lineStarts;
    constructor(buffer, lineStarts) {
      this.buffer = buffer;
      this.lineStarts = lineStarts;
    }
  };
  var PieceTreeSnapshot = class {
    _pieces;
    _index;
    _tree;
    _BOM;
    constructor(tree, BOM) {
      this._pieces = [];
      this._tree = tree;
      this._BOM = BOM;
      this._index = 0;
      if (tree.root !== SENTINEL) {
        tree.iterate(tree.root, (node) => {
          if (node !== SENTINEL) {
            this._pieces.push(node.piece);
          }
          return true;
        });
      }
    }
    read() {
      if (this._pieces.length === 0) {
        if (this._index === 0) {
          this._index++;
          return this._BOM;
        } else {
          return null;
        }
      }
      if (this._index > this._pieces.length - 1) {
        return null;
      }
      if (this._index === 0) {
        return this._BOM + this._tree.getPieceContent(this._pieces[this._index++]);
      }
      return this._tree.getPieceContent(this._pieces[this._index++]);
    }
  };
  var PieceTreeSearchCache = class {
    _limit;
    _cache;
    constructor(limit) {
      this._limit = limit;
      this._cache = [];
    }
    get(offset) {
      for (let i = this._cache.length - 1; i >= 0; i--) {
        const nodePos = this._cache[i];
        if (nodePos.nodeStartOffset <= offset && nodePos.nodeStartOffset + nodePos.node.piece.length >= offset) {
          return nodePos;
        }
      }
      return null;
    }
    get2(lineNumber) {
      for (let i = this._cache.length - 1; i >= 0; i--) {
        const nodePos = this._cache[i];
        if (nodePos.nodeStartLineNumber && nodePos.nodeStartLineNumber < lineNumber && nodePos.nodeStartLineNumber + nodePos.node.piece.lineFeedCnt >= lineNumber) {
          return nodePos;
        }
      }
      return null;
    }
    set(nodePosition) {
      if (this._cache.length >= this._limit) {
        this._cache.shift();
      }
      this._cache.push(nodePosition);
    }
    validate(offset) {
      let hasInvalidVal = false;
      const tmp = this._cache;
      for (let i = 0; i < tmp.length; i++) {
        const nodePos = tmp[i];
        if (nodePos.node.parent === null || nodePos.nodeStartOffset >= offset) {
          tmp[i] = null;
          hasInvalidVal = true;
          continue;
        }
      }
      if (hasInvalidVal) {
        const newArr = [];
        for (const entry of tmp) {
          if (entry !== null) {
            newArr.push(entry);
          }
        }
        this._cache = newArr;
      }
    }
  };
  var PieceTreeBase = class {
    root;
    _buffers;
    // 0 is change buffer, others are readonly original buffer.
    _lineCnt;
    _length;
    _EOL;
    _EOLLength;
    _EOLNormalized;
    _lastChangeBufferPos;
    _searchCache;
    _lastVisitedLine;
    constructor(chunks, eol, eolNormalized) {
      this.create(chunks, eol, eolNormalized);
    }
    create(chunks, eol, eolNormalized) {
      this._buffers = [
        new StringBuffer("", [0])
      ];
      this._lastChangeBufferPos = { line: 0, column: 0 };
      this.root = SENTINEL;
      this._lineCnt = 1;
      this._length = 0;
      this._EOL = eol;
      this._EOLLength = eol.length;
      this._EOLNormalized = eolNormalized;
      let lastNode = null;
      for (let i = 0, len = chunks.length; i < len; i++) {
        if (chunks[i].buffer.length > 0) {
          if (!chunks[i].lineStarts) {
            chunks[i].lineStarts = createLineStartsFast(chunks[i].buffer);
          }
          const piece = new Piece(
            i + 1,
            { line: 0, column: 0 },
            { line: chunks[i].lineStarts.length - 1, column: chunks[i].buffer.length - chunks[i].lineStarts[chunks[i].lineStarts.length - 1] },
            chunks[i].lineStarts.length - 1,
            chunks[i].buffer.length
          );
          this._buffers.push(chunks[i]);
          lastNode = this.rbInsertRight(lastNode, piece);
        }
      }
      this._searchCache = new PieceTreeSearchCache(1);
      this._lastVisitedLine = { lineNumber: 0, value: "" };
      this.computeBufferMetadata();
    }
    normalizeEOL(eol) {
      const averageBufferSize = AverageBufferSize;
      const min = averageBufferSize - Math.floor(averageBufferSize / 3);
      const max = min * 2;
      let tempChunk = "";
      let tempChunkLen = 0;
      const chunks = [];
      this.iterate(this.root, (node) => {
        const str = this.getNodeContent(node);
        const len = str.length;
        if (tempChunkLen <= min || tempChunkLen + len < max) {
          tempChunk += str;
          tempChunkLen += len;
          return true;
        }
        const text = tempChunk.replace(/\r\n|\r|\n/g, eol);
        chunks.push(new StringBuffer(text, createLineStartsFast(text)));
        tempChunk = str;
        tempChunkLen = len;
        return true;
      });
      if (tempChunkLen > 0) {
        const text = tempChunk.replace(/\r\n|\r|\n/g, eol);
        chunks.push(new StringBuffer(text, createLineStartsFast(text)));
      }
      this.create(chunks, eol, true);
    }
    // #region Buffer API
    getEOL() {
      return this._EOL;
    }
    setEOL(newEOL) {
      this._EOL = newEOL;
      this._EOLLength = this._EOL.length;
      this.normalizeEOL(newEOL);
    }
    createSnapshot(BOM) {
      return new PieceTreeSnapshot(this, BOM);
    }
    equal(other) {
      if (this.getLength() !== other.getLength()) {
        return false;
      }
      if (this.getLineCount() !== other.getLineCount()) {
        return false;
      }
      let offset = 0;
      const ret = this.iterate(this.root, (node) => {
        if (node === SENTINEL) {
          return true;
        }
        const str = this.getNodeContent(node);
        const len = str.length;
        const startPosition = other.nodeAt(offset);
        const endPosition = other.nodeAt(offset + len);
        const val = other.getValueInRange2(startPosition, endPosition);
        offset += len;
        return str === val;
      });
      return ret;
    }
    getOffsetAt(lineNumber, column) {
      let leftLen = 0;
      let x = this.root;
      while (x !== SENTINEL) {
        if (x.left !== SENTINEL && x.lf_left + 1 >= lineNumber) {
          x = x.left;
        } else if (x.lf_left + x.piece.lineFeedCnt + 1 >= lineNumber) {
          leftLen += x.size_left;
          const accumualtedValInCurrentIndex = this.getAccumulatedValue(x, lineNumber - x.lf_left - 2);
          return leftLen += accumualtedValInCurrentIndex + column - 1;
        } else {
          lineNumber -= x.lf_left + x.piece.lineFeedCnt;
          leftLen += x.size_left + x.piece.length;
          x = x.right;
        }
      }
      return leftLen;
    }
    getPositionAt(offset) {
      offset = Math.floor(offset);
      offset = Math.max(0, offset);
      let x = this.root;
      let lfCnt = 0;
      const originalOffset = offset;
      while (x !== SENTINEL) {
        if (x.size_left !== 0 && x.size_left >= offset) {
          x = x.left;
        } else if (x.size_left + x.piece.length >= offset) {
          const out = this.getIndexOf(x, offset - x.size_left);
          lfCnt += x.lf_left + out.index;
          if (out.index === 0) {
            const lineStartOffset = this.getOffsetAt(lfCnt + 1, 1);
            const column = originalOffset - lineStartOffset;
            return new Position(lfCnt + 1, column + 1);
          }
          return new Position(lfCnt + 1, out.remainder + 1);
        } else {
          offset -= x.size_left + x.piece.length;
          lfCnt += x.lf_left + x.piece.lineFeedCnt;
          if (x.right === SENTINEL) {
            const lineStartOffset = this.getOffsetAt(lfCnt + 1, 1);
            const column = originalOffset - offset - lineStartOffset;
            return new Position(lfCnt + 1, column + 1);
          } else {
            x = x.right;
          }
        }
      }
      return new Position(1, 1);
    }
    getValueInRange(range, eol) {
      if (range.startLineNumber === range.endLineNumber && range.startColumn === range.endColumn) {
        return "";
      }
      const startPosition = this.nodeAt2(range.startLineNumber, range.startColumn);
      const endPosition = this.nodeAt2(range.endLineNumber, range.endColumn);
      const value = this.getValueInRange2(startPosition, endPosition);
      if (eol) {
        if (eol !== this._EOL || !this._EOLNormalized) {
          return value.replace(/\r\n|\r|\n/g, eol);
        }
        if (eol === this.getEOL() && this._EOLNormalized) {
          if (eol === "\r\n") {
          }
          return value;
        }
        return value.replace(/\r\n|\r|\n/g, eol);
      }
      return value;
    }
    getValueInRange2(startPosition, endPosition) {
      if (startPosition.node === endPosition.node) {
        const node = startPosition.node;
        const buffer2 = this._buffers[node.piece.bufferIndex].buffer;
        const startOffset2 = this.offsetInBuffer(node.piece.bufferIndex, node.piece.start);
        return buffer2.substring(startOffset2 + startPosition.remainder, startOffset2 + endPosition.remainder);
      }
      let x = startPosition.node;
      const buffer = this._buffers[x.piece.bufferIndex].buffer;
      const startOffset = this.offsetInBuffer(x.piece.bufferIndex, x.piece.start);
      let ret = buffer.substring(startOffset + startPosition.remainder, startOffset + x.piece.length);
      x = x.next();
      while (x !== SENTINEL) {
        const buffer2 = this._buffers[x.piece.bufferIndex].buffer;
        const startOffset2 = this.offsetInBuffer(x.piece.bufferIndex, x.piece.start);
        if (x === endPosition.node) {
          ret += buffer2.substring(startOffset2, startOffset2 + endPosition.remainder);
          break;
        } else {
          ret += buffer2.substr(startOffset2, x.piece.length);
        }
        x = x.next();
      }
      return ret;
    }
    getLinesContent() {
      const lines = [];
      let linesLength = 0;
      let currentLine = "";
      let danglingCR = false;
      this.iterate(this.root, (node) => {
        if (node === SENTINEL) {
          return true;
        }
        const piece = node.piece;
        let pieceLength = piece.length;
        if (pieceLength === 0) {
          return true;
        }
        const buffer = this._buffers[piece.bufferIndex].buffer;
        const lineStarts = this._buffers[piece.bufferIndex].lineStarts;
        const pieceStartLine = piece.start.line;
        const pieceEndLine = piece.end.line;
        let pieceStartOffset = lineStarts[pieceStartLine] + piece.start.column;
        if (danglingCR) {
          if (buffer.charCodeAt(pieceStartOffset) === 10 /* LineFeed */) {
            pieceStartOffset++;
            pieceLength--;
          }
          lines[linesLength++] = currentLine;
          currentLine = "";
          danglingCR = false;
          if (pieceLength === 0) {
            return true;
          }
        }
        if (pieceStartLine === pieceEndLine) {
          if (!this._EOLNormalized && buffer.charCodeAt(pieceStartOffset + pieceLength - 1) === 13 /* CarriageReturn */) {
            danglingCR = true;
            currentLine += buffer.substr(pieceStartOffset, pieceLength - 1);
          } else {
            currentLine += buffer.substr(pieceStartOffset, pieceLength);
          }
          return true;
        }
        currentLine += this._EOLNormalized ? buffer.substring(pieceStartOffset, Math.max(pieceStartOffset, lineStarts[pieceStartLine + 1] - this._EOLLength)) : buffer.substring(pieceStartOffset, lineStarts[pieceStartLine + 1]).replace(/(\r\n|\r|\n)$/, "");
        lines[linesLength++] = currentLine;
        for (let line = pieceStartLine + 1; line < pieceEndLine; line++) {
          currentLine = this._EOLNormalized ? buffer.substring(lineStarts[line], lineStarts[line + 1] - this._EOLLength) : buffer.substring(lineStarts[line], lineStarts[line + 1]).replace(/(\r\n|\r|\n)$/, "");
          lines[linesLength++] = currentLine;
        }
        if (!this._EOLNormalized && buffer.charCodeAt(lineStarts[pieceEndLine] + piece.end.column - 1) === 13 /* CarriageReturn */) {
          danglingCR = true;
          if (piece.end.column === 0) {
            linesLength--;
          } else {
            currentLine = buffer.substr(lineStarts[pieceEndLine], piece.end.column - 1);
          }
        } else {
          currentLine = buffer.substr(lineStarts[pieceEndLine], piece.end.column);
        }
        return true;
      });
      if (danglingCR) {
        lines[linesLength++] = currentLine;
        currentLine = "";
      }
      lines[linesLength++] = currentLine;
      return lines;
    }
    getLength() {
      return this._length;
    }
    getLineCount() {
      return this._lineCnt;
    }
    getLineContent(lineNumber) {
      if (this._lastVisitedLine.lineNumber === lineNumber) {
        return this._lastVisitedLine.value;
      }
      this._lastVisitedLine.lineNumber = lineNumber;
      if (lineNumber === this._lineCnt) {
        this._lastVisitedLine.value = this.getLineRawContent(lineNumber);
      } else if (this._EOLNormalized) {
        this._lastVisitedLine.value = this.getLineRawContent(lineNumber, this._EOLLength);
      } else {
        this._lastVisitedLine.value = this.getLineRawContent(lineNumber).replace(/(\r\n|\r|\n)$/, "");
      }
      return this._lastVisitedLine.value;
    }
    _getCharCode(nodePos) {
      if (nodePos.remainder === nodePos.node.piece.length) {
        const matchingNode = nodePos.node.next();
        if (!matchingNode) {
          return 0;
        }
        const buffer = this._buffers[matchingNode.piece.bufferIndex];
        const startOffset = this.offsetInBuffer(matchingNode.piece.bufferIndex, matchingNode.piece.start);
        return buffer.buffer.charCodeAt(startOffset);
      } else {
        const buffer = this._buffers[nodePos.node.piece.bufferIndex];
        const startOffset = this.offsetInBuffer(nodePos.node.piece.bufferIndex, nodePos.node.piece.start);
        const targetOffset = startOffset + nodePos.remainder;
        return buffer.buffer.charCodeAt(targetOffset);
      }
    }
    getLineCharCode(lineNumber, index) {
      const nodePos = this.nodeAt2(lineNumber, index + 1);
      return this._getCharCode(nodePos);
    }
    getLineLength(lineNumber) {
      if (lineNumber === this.getLineCount()) {
        const startOffset = this.getOffsetAt(lineNumber, 1);
        return this.getLength() - startOffset;
      }
      return this.getOffsetAt(lineNumber + 1, 1) - this.getOffsetAt(lineNumber, 1) - this._EOLLength;
    }
    getCharCode(offset) {
      const nodePos = this.nodeAt(offset);
      return this._getCharCode(nodePos);
    }
    getNearestChunk(offset) {
      const nodePos = this.nodeAt(offset);
      if (nodePos.remainder === nodePos.node.piece.length) {
        const matchingNode = nodePos.node.next();
        if (!matchingNode || matchingNode === SENTINEL) {
          return "";
        }
        const buffer = this._buffers[matchingNode.piece.bufferIndex];
        const startOffset = this.offsetInBuffer(matchingNode.piece.bufferIndex, matchingNode.piece.start);
        return buffer.buffer.substring(startOffset, startOffset + matchingNode.piece.length);
      } else {
        const buffer = this._buffers[nodePos.node.piece.bufferIndex];
        const startOffset = this.offsetInBuffer(nodePos.node.piece.bufferIndex, nodePos.node.piece.start);
        const targetOffset = startOffset + nodePos.remainder;
        const targetEnd = startOffset + nodePos.node.piece.length;
        return buffer.buffer.substring(targetOffset, targetEnd);
      }
    }
    findMatchesInNode(node, searcher, startLineNumber, startColumn, startCursor, endCursor, searchData, captureMatches, limitResultCount, resultLen, result) {
      const buffer = this._buffers[node.piece.bufferIndex];
      const startOffsetInBuffer = this.offsetInBuffer(node.piece.bufferIndex, node.piece.start);
      const start = this.offsetInBuffer(node.piece.bufferIndex, startCursor);
      const end = this.offsetInBuffer(node.piece.bufferIndex, endCursor);
      let m;
      const ret = { line: 0, column: 0 };
      let searchText;
      let offsetInBuffer;
      if (searcher._wordSeparators) {
        searchText = buffer.buffer.substring(start, end);
        offsetInBuffer = (offset) => offset + start;
        searcher.reset(0);
      } else {
        searchText = buffer.buffer;
        offsetInBuffer = (offset) => offset;
        searcher.reset(start);
      }
      do {
        m = searcher.next(searchText);
        if (m) {
          if (offsetInBuffer(m.index) >= end) {
            return resultLen;
          }
          this.positionInBuffer(node, offsetInBuffer(m.index) - startOffsetInBuffer, ret);
          const lineFeedCnt = this.getLineFeedCnt(node.piece.bufferIndex, startCursor, ret);
          const retStartColumn = ret.line === startCursor.line ? ret.column - startCursor.column + startColumn : ret.column + 1;
          const retEndColumn = retStartColumn + m[0].length;
          result[resultLen++] = createFindMatch(new Range(startLineNumber + lineFeedCnt, retStartColumn, startLineNumber + lineFeedCnt, retEndColumn), m, captureMatches);
          if (offsetInBuffer(m.index) + m[0].length >= end) {
            return resultLen;
          }
          if (resultLen >= limitResultCount) {
            return resultLen;
          }
        }
      } while (m);
      return resultLen;
    }
    findMatchesLineByLine(searchRange, searchData, captureMatches, limitResultCount) {
      const result = [];
      let resultLen = 0;
      const searcher = new Searcher(searchData.wordSeparators, searchData.regex);
      let startPosition = this.nodeAt2(searchRange.startLineNumber, searchRange.startColumn);
      if (startPosition === null) {
        return [];
      }
      const endPosition = this.nodeAt2(searchRange.endLineNumber, searchRange.endColumn);
      if (endPosition === null) {
        return [];
      }
      let start = this.positionInBuffer(startPosition.node, startPosition.remainder);
      const end = this.positionInBuffer(endPosition.node, endPosition.remainder);
      if (startPosition.node === endPosition.node) {
        this.findMatchesInNode(startPosition.node, searcher, searchRange.startLineNumber, searchRange.startColumn, start, end, searchData, captureMatches, limitResultCount, resultLen, result);
        return result;
      }
      let startLineNumber = searchRange.startLineNumber;
      let currentNode = startPosition.node;
      while (currentNode !== endPosition.node) {
        const lineBreakCnt = this.getLineFeedCnt(currentNode.piece.bufferIndex, start, currentNode.piece.end);
        if (lineBreakCnt >= 1) {
          const lineStarts = this._buffers[currentNode.piece.bufferIndex].lineStarts;
          const startOffsetInBuffer = this.offsetInBuffer(currentNode.piece.bufferIndex, currentNode.piece.start);
          const nextLineStartOffset = lineStarts[start.line + lineBreakCnt];
          const startColumn3 = startLineNumber === searchRange.startLineNumber ? searchRange.startColumn : 1;
          resultLen = this.findMatchesInNode(currentNode, searcher, startLineNumber, startColumn3, start, this.positionInBuffer(currentNode, nextLineStartOffset - startOffsetInBuffer), searchData, captureMatches, limitResultCount, resultLen, result);
          if (resultLen >= limitResultCount) {
            return result;
          }
          startLineNumber += lineBreakCnt;
        }
        const startColumn2 = startLineNumber === searchRange.startLineNumber ? searchRange.startColumn - 1 : 0;
        if (startLineNumber === searchRange.endLineNumber) {
          const text = this.getLineContent(startLineNumber).substring(startColumn2, searchRange.endColumn - 1);
          resultLen = this._findMatchesInLine(searchData, searcher, text, searchRange.endLineNumber, startColumn2, resultLen, result, captureMatches, limitResultCount);
          return result;
        }
        resultLen = this._findMatchesInLine(searchData, searcher, this.getLineContent(startLineNumber).substr(startColumn2), startLineNumber, startColumn2, resultLen, result, captureMatches, limitResultCount);
        if (resultLen >= limitResultCount) {
          return result;
        }
        startLineNumber++;
        startPosition = this.nodeAt2(startLineNumber, 1);
        currentNode = startPosition.node;
        start = this.positionInBuffer(startPosition.node, startPosition.remainder);
      }
      if (startLineNumber === searchRange.endLineNumber) {
        const startColumn2 = startLineNumber === searchRange.startLineNumber ? searchRange.startColumn - 1 : 0;
        const text = this.getLineContent(startLineNumber).substring(startColumn2, searchRange.endColumn - 1);
        resultLen = this._findMatchesInLine(searchData, searcher, text, searchRange.endLineNumber, startColumn2, resultLen, result, captureMatches, limitResultCount);
        return result;
      }
      const startColumn = startLineNumber === searchRange.startLineNumber ? searchRange.startColumn : 1;
      resultLen = this.findMatchesInNode(endPosition.node, searcher, startLineNumber, startColumn, start, end, searchData, captureMatches, limitResultCount, resultLen, result);
      return result;
    }
    _findMatchesInLine(searchData, searcher, text, lineNumber, deltaOffset, resultLen, result, captureMatches, limitResultCount) {
      const wordSeparators = searchData.wordSeparators;
      if (!captureMatches && searchData.simpleSearch) {
        const searchString = searchData.simpleSearch;
        const searchStringLen = searchString.length;
        const textLength = text.length;
        let lastMatchIndex = -searchStringLen;
        while ((lastMatchIndex = text.indexOf(searchString, lastMatchIndex + searchStringLen)) !== -1) {
          if (!wordSeparators || isValidMatch(wordSeparators, text, textLength, lastMatchIndex, searchStringLen)) {
            result[resultLen++] = new FindMatch(new Range(lineNumber, lastMatchIndex + 1 + deltaOffset, lineNumber, lastMatchIndex + 1 + searchStringLen + deltaOffset), null);
            if (resultLen >= limitResultCount) {
              return resultLen;
            }
          }
        }
        return resultLen;
      }
      let m;
      searcher.reset(0);
      do {
        m = searcher.next(text);
        if (m) {
          result[resultLen++] = createFindMatch(new Range(lineNumber, m.index + 1 + deltaOffset, lineNumber, m.index + 1 + m[0].length + deltaOffset), m, captureMatches);
          if (resultLen >= limitResultCount) {
            return resultLen;
          }
        }
      } while (m);
      return resultLen;
    }
    // #endregion
    // #region Piece Table
    insert(offset, value, eolNormalized = false) {
      this._EOLNormalized = this._EOLNormalized && eolNormalized;
      this._lastVisitedLine.lineNumber = 0;
      this._lastVisitedLine.value = "";
      if (this.root !== SENTINEL) {
        const { node, remainder, nodeStartOffset } = this.nodeAt(offset);
        const piece = node.piece;
        const bufferIndex = piece.bufferIndex;
        const insertPosInBuffer = this.positionInBuffer(node, remainder);
        if (node.piece.bufferIndex === 0 && piece.end.line === this._lastChangeBufferPos.line && piece.end.column === this._lastChangeBufferPos.column && nodeStartOffset + piece.length === offset && value.length < AverageBufferSize) {
          this.appendToNode(node, value);
          this.computeBufferMetadata();
          return;
        }
        if (nodeStartOffset === offset) {
          this.insertContentToNodeLeft(value, node);
          this._searchCache.validate(offset);
        } else if (nodeStartOffset + node.piece.length > offset) {
          const nodesToDel = [];
          let newRightPiece = new Piece(
            piece.bufferIndex,
            insertPosInBuffer,
            piece.end,
            this.getLineFeedCnt(piece.bufferIndex, insertPosInBuffer, piece.end),
            this.offsetInBuffer(bufferIndex, piece.end) - this.offsetInBuffer(bufferIndex, insertPosInBuffer)
          );
          if (this.shouldCheckCRLF() && this.endWithCR(value)) {
            const headOfRight = this.nodeCharCodeAt(node, remainder);
            if (headOfRight === 10) {
              const newStart = { line: newRightPiece.start.line + 1, column: 0 };
              newRightPiece = new Piece(
                newRightPiece.bufferIndex,
                newStart,
                newRightPiece.end,
                this.getLineFeedCnt(newRightPiece.bufferIndex, newStart, newRightPiece.end),
                newRightPiece.length - 1
              );
              value += "\n";
            }
          }
          if (this.shouldCheckCRLF() && this.startWithLF(value)) {
            const tailOfLeft = this.nodeCharCodeAt(node, remainder - 1);
            if (tailOfLeft === 13) {
              const previousPos = this.positionInBuffer(node, remainder - 1);
              this.deleteNodeTail(node, previousPos);
              value = "\r" + value;
              if (node.piece.length === 0) {
                nodesToDel.push(node);
              }
            } else {
              this.deleteNodeTail(node, insertPosInBuffer);
            }
          } else {
            this.deleteNodeTail(node, insertPosInBuffer);
          }
          const newPieces = this.createNewPieces(value);
          if (newRightPiece.length > 0) {
            this.rbInsertRight(node, newRightPiece);
          }
          let tmpNode = node;
          for (let k = 0; k < newPieces.length; k++) {
            tmpNode = this.rbInsertRight(tmpNode, newPieces[k]);
          }
          this.deleteNodes(nodesToDel);
        } else {
          this.insertContentToNodeRight(value, node);
        }
      } else {
        const pieces = this.createNewPieces(value);
        let node = this.rbInsertLeft(null, pieces[0]);
        for (let k = 1; k < pieces.length; k++) {
          node = this.rbInsertRight(node, pieces[k]);
        }
      }
      this.computeBufferMetadata();
    }
    delete(offset, cnt) {
      this._lastVisitedLine.lineNumber = 0;
      this._lastVisitedLine.value = "";
      if (cnt <= 0 || this.root === SENTINEL) {
        return;
      }
      const startPosition = this.nodeAt(offset);
      const endPosition = this.nodeAt(offset + cnt);
      const startNode = startPosition.node;
      const endNode = endPosition.node;
      if (startNode === endNode) {
        const startSplitPosInBuffer2 = this.positionInBuffer(startNode, startPosition.remainder);
        const endSplitPosInBuffer2 = this.positionInBuffer(startNode, endPosition.remainder);
        if (startPosition.nodeStartOffset === offset) {
          if (cnt === startNode.piece.length) {
            const next2 = startNode.next();
            rbDelete(this, startNode);
            this.validateCRLFWithPrevNode(next2);
            this.computeBufferMetadata();
            return;
          }
          this.deleteNodeHead(startNode, endSplitPosInBuffer2);
          this._searchCache.validate(offset);
          this.validateCRLFWithPrevNode(startNode);
          this.computeBufferMetadata();
          return;
        }
        if (startPosition.nodeStartOffset + startNode.piece.length === offset + cnt) {
          this.deleteNodeTail(startNode, startSplitPosInBuffer2);
          this.validateCRLFWithNextNode(startNode);
          this.computeBufferMetadata();
          return;
        }
        this.shrinkNode(startNode, startSplitPosInBuffer2, endSplitPosInBuffer2);
        this.computeBufferMetadata();
        return;
      }
      const nodesToDel = [];
      const startSplitPosInBuffer = this.positionInBuffer(startNode, startPosition.remainder);
      this.deleteNodeTail(startNode, startSplitPosInBuffer);
      this._searchCache.validate(offset);
      if (startNode.piece.length === 0) {
        nodesToDel.push(startNode);
      }
      const endSplitPosInBuffer = this.positionInBuffer(endNode, endPosition.remainder);
      this.deleteNodeHead(endNode, endSplitPosInBuffer);
      if (endNode.piece.length === 0) {
        nodesToDel.push(endNode);
      }
      const secondNode = startNode.next();
      for (let node = secondNode; node !== SENTINEL && node !== endNode; node = node.next()) {
        nodesToDel.push(node);
      }
      const prev2 = startNode.piece.length === 0 ? startNode.prev() : startNode;
      this.deleteNodes(nodesToDel);
      this.validateCRLFWithNextNode(prev2);
      this.computeBufferMetadata();
    }
    insertContentToNodeLeft(value, node) {
      const nodesToDel = [];
      if (this.shouldCheckCRLF() && this.endWithCR(value) && this.startWithLF(node)) {
        const piece = node.piece;
        const newStart = { line: piece.start.line + 1, column: 0 };
        const nPiece = new Piece(
          piece.bufferIndex,
          newStart,
          piece.end,
          this.getLineFeedCnt(piece.bufferIndex, newStart, piece.end),
          piece.length - 1
        );
        node.piece = nPiece;
        value += "\n";
        updateTreeMetadata(this, node, -1, -1);
        if (node.piece.length === 0) {
          nodesToDel.push(node);
        }
      }
      const newPieces = this.createNewPieces(value);
      let newNode = this.rbInsertLeft(node, newPieces[newPieces.length - 1]);
      for (let k = newPieces.length - 2; k >= 0; k--) {
        newNode = this.rbInsertLeft(newNode, newPieces[k]);
      }
      this.validateCRLFWithPrevNode(newNode);
      this.deleteNodes(nodesToDel);
    }
    insertContentToNodeRight(value, node) {
      if (this.adjustCarriageReturnFromNext(value, node)) {
        value += "\n";
      }
      const newPieces = this.createNewPieces(value);
      const newNode = this.rbInsertRight(node, newPieces[0]);
      let tmpNode = newNode;
      for (let k = 1; k < newPieces.length; k++) {
        tmpNode = this.rbInsertRight(tmpNode, newPieces[k]);
      }
      this.validateCRLFWithPrevNode(newNode);
    }
    positionInBuffer(node, remainder, ret) {
      const piece = node.piece;
      const bufferIndex = node.piece.bufferIndex;
      const lineStarts = this._buffers[bufferIndex].lineStarts;
      const startOffset = lineStarts[piece.start.line] + piece.start.column;
      const offset = startOffset + remainder;
      let low = piece.start.line;
      let high = piece.end.line;
      let mid = 0;
      let midStop = 0;
      let midStart = 0;
      while (low <= high) {
        mid = low + (high - low) / 2 | 0;
        midStart = lineStarts[mid];
        if (mid === high) {
          break;
        }
        midStop = lineStarts[mid + 1];
        if (offset < midStart) {
          high = mid - 1;
        } else if (offset >= midStop) {
          low = mid + 1;
        } else {
          break;
        }
      }
      if (ret) {
        ret.line = mid;
        ret.column = offset - midStart;
        return null;
      }
      return {
        line: mid,
        column: offset - midStart
      };
    }
    getLineFeedCnt(bufferIndex, start, end) {
      if (end.column === 0) {
        return end.line - start.line;
      }
      const lineStarts = this._buffers[bufferIndex].lineStarts;
      if (end.line === lineStarts.length - 1) {
        return end.line - start.line;
      }
      const nextLineStartOffset = lineStarts[end.line + 1];
      const endOffset = lineStarts[end.line] + end.column;
      if (nextLineStartOffset > endOffset + 1) {
        return end.line - start.line;
      }
      const previousCharOffset = endOffset - 1;
      const buffer = this._buffers[bufferIndex].buffer;
      if (buffer.charCodeAt(previousCharOffset) === 13) {
        return end.line - start.line + 1;
      } else {
        return end.line - start.line;
      }
    }
    offsetInBuffer(bufferIndex, cursor) {
      const lineStarts = this._buffers[bufferIndex].lineStarts;
      return lineStarts[cursor.line] + cursor.column;
    }
    deleteNodes(nodes) {
      for (let i = 0; i < nodes.length; i++) {
        rbDelete(this, nodes[i]);
      }
    }
    createNewPieces(text) {
      if (text.length > AverageBufferSize) {
        const newPieces = [];
        while (text.length > AverageBufferSize) {
          const lastChar = text.charCodeAt(AverageBufferSize - 1);
          let splitText;
          if (lastChar === 13 /* CarriageReturn */ || lastChar >= 55296 && lastChar <= 56319) {
            splitText = text.substring(0, AverageBufferSize - 1);
            text = text.substring(AverageBufferSize - 1);
          } else {
            splitText = text.substring(0, AverageBufferSize);
            text = text.substring(AverageBufferSize);
          }
          const lineStarts3 = createLineStartsFast(splitText);
          newPieces.push(new Piece(
            this._buffers.length,
            /* buffer index */
            { line: 0, column: 0 },
            { line: lineStarts3.length - 1, column: splitText.length - lineStarts3[lineStarts3.length - 1] },
            lineStarts3.length - 1,
            splitText.length
          ));
          this._buffers.push(new StringBuffer(splitText, lineStarts3));
        }
        const lineStarts2 = createLineStartsFast(text);
        newPieces.push(new Piece(
          this._buffers.length,
          /* buffer index */
          { line: 0, column: 0 },
          { line: lineStarts2.length - 1, column: text.length - lineStarts2[lineStarts2.length - 1] },
          lineStarts2.length - 1,
          text.length
        ));
        this._buffers.push(new StringBuffer(text, lineStarts2));
        return newPieces;
      }
      let startOffset = this._buffers[0].buffer.length;
      const lineStarts = createLineStartsFast(text, false);
      let start = this._lastChangeBufferPos;
      if (this._buffers[0].lineStarts[this._buffers[0].lineStarts.length - 1] === startOffset && startOffset !== 0 && this.startWithLF(text) && this.endWithCR(this._buffers[0].buffer)) {
        this._lastChangeBufferPos = { line: this._lastChangeBufferPos.line, column: this._lastChangeBufferPos.column + 1 };
        start = this._lastChangeBufferPos;
        for (let i = 0; i < lineStarts.length; i++) {
          lineStarts[i] += startOffset + 1;
        }
        this._buffers[0].lineStarts = this._buffers[0].lineStarts.concat(lineStarts.slice(1));
        this._buffers[0].buffer += "_" + text;
        startOffset += 1;
      } else {
        if (startOffset !== 0) {
          for (let i = 0; i < lineStarts.length; i++) {
            lineStarts[i] += startOffset;
          }
        }
        this._buffers[0].lineStarts = this._buffers[0].lineStarts.concat(lineStarts.slice(1));
        this._buffers[0].buffer += text;
      }
      const endOffset = this._buffers[0].buffer.length;
      const endIndex = this._buffers[0].lineStarts.length - 1;
      const endColumn = endOffset - this._buffers[0].lineStarts[endIndex];
      const endPos = { line: endIndex, column: endColumn };
      const newPiece = new Piece(
        0,
        /** todo@peng */
        start,
        endPos,
        this.getLineFeedCnt(0, start, endPos),
        endOffset - startOffset
      );
      this._lastChangeBufferPos = endPos;
      return [newPiece];
    }
    getLinesRawContent() {
      return this.getContentOfSubTree(this.root);
    }
    getLineRawContent(lineNumber, endOffset = 0) {
      let x = this.root;
      let ret = "";
      const cache = this._searchCache.get2(lineNumber);
      if (cache) {
        x = cache.node;
        const prevAccumulatedValue = this.getAccumulatedValue(x, lineNumber - cache.nodeStartLineNumber - 1);
        const buffer = this._buffers[x.piece.bufferIndex].buffer;
        const startOffset = this.offsetInBuffer(x.piece.bufferIndex, x.piece.start);
        if (cache.nodeStartLineNumber + x.piece.lineFeedCnt === lineNumber) {
          ret = buffer.substring(startOffset + prevAccumulatedValue, startOffset + x.piece.length);
        } else {
          const accumulatedValue = this.getAccumulatedValue(x, lineNumber - cache.nodeStartLineNumber);
          return buffer.substring(startOffset + prevAccumulatedValue, startOffset + accumulatedValue - endOffset);
        }
      } else {
        let nodeStartOffset = 0;
        const originalLineNumber = lineNumber;
        while (x !== SENTINEL) {
          if (x.left !== SENTINEL && x.lf_left >= lineNumber - 1) {
            x = x.left;
          } else if (x.lf_left + x.piece.lineFeedCnt > lineNumber - 1) {
            const prevAccumulatedValue = this.getAccumulatedValue(x, lineNumber - x.lf_left - 2);
            const accumulatedValue = this.getAccumulatedValue(x, lineNumber - x.lf_left - 1);
            const buffer = this._buffers[x.piece.bufferIndex].buffer;
            const startOffset = this.offsetInBuffer(x.piece.bufferIndex, x.piece.start);
            nodeStartOffset += x.size_left;
            this._searchCache.set({
              node: x,
              nodeStartOffset,
              nodeStartLineNumber: originalLineNumber - (lineNumber - 1 - x.lf_left)
            });
            return buffer.substring(startOffset + prevAccumulatedValue, startOffset + accumulatedValue - endOffset);
          } else if (x.lf_left + x.piece.lineFeedCnt === lineNumber - 1) {
            const prevAccumulatedValue = this.getAccumulatedValue(x, lineNumber - x.lf_left - 2);
            const buffer = this._buffers[x.piece.bufferIndex].buffer;
            const startOffset = this.offsetInBuffer(x.piece.bufferIndex, x.piece.start);
            ret = buffer.substring(startOffset + prevAccumulatedValue, startOffset + x.piece.length);
            break;
          } else {
            lineNumber -= x.lf_left + x.piece.lineFeedCnt;
            nodeStartOffset += x.size_left + x.piece.length;
            x = x.right;
          }
        }
      }
      x = x.next();
      while (x !== SENTINEL) {
        const buffer = this._buffers[x.piece.bufferIndex].buffer;
        if (x.piece.lineFeedCnt > 0) {
          const accumulatedValue = this.getAccumulatedValue(x, 0);
          const startOffset = this.offsetInBuffer(x.piece.bufferIndex, x.piece.start);
          ret += buffer.substring(startOffset, startOffset + accumulatedValue - endOffset);
          return ret;
        } else {
          const startOffset = this.offsetInBuffer(x.piece.bufferIndex, x.piece.start);
          ret += buffer.substr(startOffset, x.piece.length);
        }
        x = x.next();
      }
      return ret;
    }
    computeBufferMetadata() {
      let x = this.root;
      let lfCnt = 1;
      let len = 0;
      while (x !== SENTINEL) {
        lfCnt += x.lf_left + x.piece.lineFeedCnt;
        len += x.size_left + x.piece.length;
        x = x.right;
      }
      this._lineCnt = lfCnt;
      this._length = len;
      this._searchCache.validate(this._length);
    }
    // #region node operations
    getIndexOf(node, accumulatedValue) {
      const piece = node.piece;
      const pos = this.positionInBuffer(node, accumulatedValue);
      const lineCnt = pos.line - piece.start.line;
      if (this.offsetInBuffer(piece.bufferIndex, piece.end) - this.offsetInBuffer(piece.bufferIndex, piece.start) === accumulatedValue) {
        const realLineCnt = this.getLineFeedCnt(node.piece.bufferIndex, piece.start, pos);
        if (realLineCnt !== lineCnt) {
          return { index: realLineCnt, remainder: 0 };
        }
      }
      return { index: lineCnt, remainder: pos.column };
    }
    getAccumulatedValue(node, index) {
      if (index < 0) {
        return 0;
      }
      const piece = node.piece;
      const lineStarts = this._buffers[piece.bufferIndex].lineStarts;
      const expectedLineStartIndex = piece.start.line + index + 1;
      if (expectedLineStartIndex > piece.end.line) {
        return lineStarts[piece.end.line] + piece.end.column - lineStarts[piece.start.line] - piece.start.column;
      } else {
        return lineStarts[expectedLineStartIndex] - lineStarts[piece.start.line] - piece.start.column;
      }
    }
    deleteNodeTail(node, pos) {
      const piece = node.piece;
      const originalLFCnt = piece.lineFeedCnt;
      const originalEndOffset = this.offsetInBuffer(piece.bufferIndex, piece.end);
      const newEnd = pos;
      const newEndOffset = this.offsetInBuffer(piece.bufferIndex, newEnd);
      const newLineFeedCnt = this.getLineFeedCnt(piece.bufferIndex, piece.start, newEnd);
      const lf_delta = newLineFeedCnt - originalLFCnt;
      const size_delta = newEndOffset - originalEndOffset;
      const newLength = piece.length + size_delta;
      node.piece = new Piece(
        piece.bufferIndex,
        piece.start,
        newEnd,
        newLineFeedCnt,
        newLength
      );
      updateTreeMetadata(this, node, size_delta, lf_delta);
    }
    deleteNodeHead(node, pos) {
      const piece = node.piece;
      const originalLFCnt = piece.lineFeedCnt;
      const originalStartOffset = this.offsetInBuffer(piece.bufferIndex, piece.start);
      const newStart = pos;
      const newLineFeedCnt = this.getLineFeedCnt(piece.bufferIndex, newStart, piece.end);
      const newStartOffset = this.offsetInBuffer(piece.bufferIndex, newStart);
      const lf_delta = newLineFeedCnt - originalLFCnt;
      const size_delta = originalStartOffset - newStartOffset;
      const newLength = piece.length + size_delta;
      node.piece = new Piece(
        piece.bufferIndex,
        newStart,
        piece.end,
        newLineFeedCnt,
        newLength
      );
      updateTreeMetadata(this, node, size_delta, lf_delta);
    }
    shrinkNode(node, start, end) {
      const piece = node.piece;
      const originalStartPos = piece.start;
      const originalEndPos = piece.end;
      const oldLength = piece.length;
      const oldLFCnt = piece.lineFeedCnt;
      const newEnd = start;
      const newLineFeedCnt = this.getLineFeedCnt(piece.bufferIndex, piece.start, newEnd);
      const newLength = this.offsetInBuffer(piece.bufferIndex, start) - this.offsetInBuffer(piece.bufferIndex, originalStartPos);
      node.piece = new Piece(
        piece.bufferIndex,
        piece.start,
        newEnd,
        newLineFeedCnt,
        newLength
      );
      updateTreeMetadata(this, node, newLength - oldLength, newLineFeedCnt - oldLFCnt);
      const newPiece = new Piece(
        piece.bufferIndex,
        end,
        originalEndPos,
        this.getLineFeedCnt(piece.bufferIndex, end, originalEndPos),
        this.offsetInBuffer(piece.bufferIndex, originalEndPos) - this.offsetInBuffer(piece.bufferIndex, end)
      );
      const newNode = this.rbInsertRight(node, newPiece);
      this.validateCRLFWithPrevNode(newNode);
    }
    appendToNode(node, value) {
      if (this.adjustCarriageReturnFromNext(value, node)) {
        value += "\n";
      }
      const hitCRLF = this.shouldCheckCRLF() && this.startWithLF(value) && this.endWithCR(node);
      const startOffset = this._buffers[0].buffer.length;
      this._buffers[0].buffer += value;
      const lineStarts = createLineStartsFast(value, false);
      for (let i = 0; i < lineStarts.length; i++) {
        lineStarts[i] += startOffset;
      }
      if (hitCRLF) {
        const prevStartOffset = this._buffers[0].lineStarts[this._buffers[0].lineStarts.length - 2];
        this._buffers[0].lineStarts.pop();
        this._lastChangeBufferPos = { line: this._lastChangeBufferPos.line - 1, column: startOffset - prevStartOffset };
      }
      this._buffers[0].lineStarts = this._buffers[0].lineStarts.concat(lineStarts.slice(1));
      const endIndex = this._buffers[0].lineStarts.length - 1;
      const endColumn = this._buffers[0].buffer.length - this._buffers[0].lineStarts[endIndex];
      const newEnd = { line: endIndex, column: endColumn };
      const newLength = node.piece.length + value.length;
      const oldLineFeedCnt = node.piece.lineFeedCnt;
      const newLineFeedCnt = this.getLineFeedCnt(0, node.piece.start, newEnd);
      const lf_delta = newLineFeedCnt - oldLineFeedCnt;
      node.piece = new Piece(
        node.piece.bufferIndex,
        node.piece.start,
        newEnd,
        newLineFeedCnt,
        newLength
      );
      this._lastChangeBufferPos = newEnd;
      updateTreeMetadata(this, node, value.length, lf_delta);
    }
    nodeAt(offset) {
      let x = this.root;
      const cache = this._searchCache.get(offset);
      if (cache) {
        return {
          node: cache.node,
          nodeStartOffset: cache.nodeStartOffset,
          remainder: offset - cache.nodeStartOffset
        };
      }
      let nodeStartOffset = 0;
      while (x !== SENTINEL) {
        if (x.size_left > offset) {
          x = x.left;
        } else if (x.size_left + x.piece.length >= offset) {
          nodeStartOffset += x.size_left;
          const ret = {
            node: x,
            remainder: offset - x.size_left,
            nodeStartOffset
          };
          this._searchCache.set(ret);
          return ret;
        } else {
          offset -= x.size_left + x.piece.length;
          nodeStartOffset += x.size_left + x.piece.length;
          x = x.right;
        }
      }
      return null;
    }
    nodeAt2(lineNumber, column) {
      let x = this.root;
      let nodeStartOffset = 0;
      while (x !== SENTINEL) {
        if (x.left !== SENTINEL && x.lf_left >= lineNumber - 1) {
          x = x.left;
        } else if (x.lf_left + x.piece.lineFeedCnt > lineNumber - 1) {
          const prevAccumualtedValue = this.getAccumulatedValue(x, lineNumber - x.lf_left - 2);
          const accumulatedValue = this.getAccumulatedValue(x, lineNumber - x.lf_left - 1);
          nodeStartOffset += x.size_left;
          return {
            node: x,
            remainder: Math.min(prevAccumualtedValue + column - 1, accumulatedValue),
            nodeStartOffset
          };
        } else if (x.lf_left + x.piece.lineFeedCnt === lineNumber - 1) {
          const prevAccumualtedValue = this.getAccumulatedValue(x, lineNumber - x.lf_left - 2);
          if (prevAccumualtedValue + column - 1 <= x.piece.length) {
            return {
              node: x,
              remainder: prevAccumualtedValue + column - 1,
              nodeStartOffset
            };
          } else {
            column -= x.piece.length - prevAccumualtedValue;
            break;
          }
        } else {
          lineNumber -= x.lf_left + x.piece.lineFeedCnt;
          nodeStartOffset += x.size_left + x.piece.length;
          x = x.right;
        }
      }
      x = x.next();
      while (x !== SENTINEL) {
        if (x.piece.lineFeedCnt > 0) {
          const accumulatedValue = this.getAccumulatedValue(x, 0);
          const nodeStartOffset2 = this.offsetOfNode(x);
          return {
            node: x,
            remainder: Math.min(column - 1, accumulatedValue),
            nodeStartOffset: nodeStartOffset2
          };
        } else {
          if (x.piece.length >= column - 1) {
            const nodeStartOffset2 = this.offsetOfNode(x);
            return {
              node: x,
              remainder: column - 1,
              nodeStartOffset: nodeStartOffset2
            };
          } else {
            column -= x.piece.length;
          }
        }
        x = x.next();
      }
      return null;
    }
    nodeCharCodeAt(node, offset) {
      if (node.piece.lineFeedCnt < 1) {
        return -1;
      }
      const buffer = this._buffers[node.piece.bufferIndex];
      const newOffset = this.offsetInBuffer(node.piece.bufferIndex, node.piece.start) + offset;
      return buffer.buffer.charCodeAt(newOffset);
    }
    offsetOfNode(node) {
      if (!node) {
        return 0;
      }
      let pos = node.size_left;
      while (node !== this.root) {
        if (node.parent.right === node) {
          pos += node.parent.size_left + node.parent.piece.length;
        }
        node = node.parent;
      }
      return pos;
    }
    // #endregion
    // #region CRLF
    shouldCheckCRLF() {
      return !(this._EOLNormalized && this._EOL === "\n");
    }
    startWithLF(val) {
      if (typeof val === "string") {
        return val.charCodeAt(0) === 10;
      }
      if (val === SENTINEL || val.piece.lineFeedCnt === 0) {
        return false;
      }
      const piece = val.piece;
      const lineStarts = this._buffers[piece.bufferIndex].lineStarts;
      const line = piece.start.line;
      const startOffset = lineStarts[line] + piece.start.column;
      if (line === lineStarts.length - 1) {
        return false;
      }
      const nextLineOffset = lineStarts[line + 1];
      if (nextLineOffset > startOffset + 1) {
        return false;
      }
      return this._buffers[piece.bufferIndex].buffer.charCodeAt(startOffset) === 10;
    }
    endWithCR(val) {
      if (typeof val === "string") {
        return val.charCodeAt(val.length - 1) === 13;
      }
      if (val === SENTINEL || val.piece.lineFeedCnt === 0) {
        return false;
      }
      return this.nodeCharCodeAt(val, val.piece.length - 1) === 13;
    }
    validateCRLFWithPrevNode(nextNode) {
      if (this.shouldCheckCRLF() && this.startWithLF(nextNode)) {
        const node = nextNode.prev();
        if (this.endWithCR(node)) {
          this.fixCRLF(node, nextNode);
        }
      }
    }
    validateCRLFWithNextNode(node) {
      if (this.shouldCheckCRLF() && this.endWithCR(node)) {
        const nextNode = node.next();
        if (this.startWithLF(nextNode)) {
          this.fixCRLF(node, nextNode);
        }
      }
    }
    fixCRLF(prev2, next2) {
      const nodesToDel = [];
      const lineStarts = this._buffers[prev2.piece.bufferIndex].lineStarts;
      let newEnd;
      if (prev2.piece.end.column === 0) {
        newEnd = { line: prev2.piece.end.line - 1, column: lineStarts[prev2.piece.end.line] - lineStarts[prev2.piece.end.line - 1] - 1 };
      } else {
        newEnd = { line: prev2.piece.end.line, column: prev2.piece.end.column - 1 };
      }
      const prevNewLength = prev2.piece.length - 1;
      const prevNewLFCnt = prev2.piece.lineFeedCnt - 1;
      prev2.piece = new Piece(
        prev2.piece.bufferIndex,
        prev2.piece.start,
        newEnd,
        prevNewLFCnt,
        prevNewLength
      );
      updateTreeMetadata(this, prev2, -1, -1);
      if (prev2.piece.length === 0) {
        nodesToDel.push(prev2);
      }
      const newStart = { line: next2.piece.start.line + 1, column: 0 };
      const newLength = next2.piece.length - 1;
      const newLineFeedCnt = this.getLineFeedCnt(next2.piece.bufferIndex, newStart, next2.piece.end);
      next2.piece = new Piece(
        next2.piece.bufferIndex,
        newStart,
        next2.piece.end,
        newLineFeedCnt,
        newLength
      );
      updateTreeMetadata(this, next2, -1, -1);
      if (next2.piece.length === 0) {
        nodesToDel.push(next2);
      }
      const pieces = this.createNewPieces("\r\n");
      this.rbInsertRight(prev2, pieces[0]);
      for (let i = 0; i < nodesToDel.length; i++) {
        rbDelete(this, nodesToDel[i]);
      }
    }
    adjustCarriageReturnFromNext(value, node) {
      if (this.shouldCheckCRLF() && this.endWithCR(value)) {
        const nextNode = node.next();
        if (this.startWithLF(nextNode)) {
          value += "\n";
          if (nextNode.piece.length === 1) {
            rbDelete(this, nextNode);
          } else {
            const piece = nextNode.piece;
            const newStart = { line: piece.start.line + 1, column: 0 };
            const newLength = piece.length - 1;
            const newLineFeedCnt = this.getLineFeedCnt(piece.bufferIndex, newStart, piece.end);
            nextNode.piece = new Piece(
              piece.bufferIndex,
              newStart,
              piece.end,
              newLineFeedCnt,
              newLength
            );
            updateTreeMetadata(this, nextNode, -1, -1);
          }
          return true;
        }
      }
      return false;
    }
    // #endregion
    // #endregion
    // #region Tree operations
    iterate(node, callback) {
      if (node === SENTINEL) {
        return callback(SENTINEL);
      }
      const leftRet = this.iterate(node.left, callback);
      if (!leftRet) {
        return leftRet;
      }
      return callback(node) && this.iterate(node.right, callback);
    }
    getNodeContent(node) {
      if (node === SENTINEL) {
        return "";
      }
      const buffer = this._buffers[node.piece.bufferIndex];
      const piece = node.piece;
      const startOffset = this.offsetInBuffer(piece.bufferIndex, piece.start);
      const endOffset = this.offsetInBuffer(piece.bufferIndex, piece.end);
      const currentContent = buffer.buffer.substring(startOffset, endOffset);
      return currentContent;
    }
    getPieceContent(piece) {
      const buffer = this._buffers[piece.bufferIndex];
      const startOffset = this.offsetInBuffer(piece.bufferIndex, piece.start);
      const endOffset = this.offsetInBuffer(piece.bufferIndex, piece.end);
      const currentContent = buffer.buffer.substring(startOffset, endOffset);
      return currentContent;
    }
    /**
     *      node              node
     *     /  \              /  \
     *    a   b    <----   a    b
     *                         /
     *                        z
     */
    rbInsertRight(node, p) {
      const z = new TreeNode(p, 1 /* Red */);
      z.left = SENTINEL;
      z.right = SENTINEL;
      z.parent = SENTINEL;
      z.size_left = 0;
      z.lf_left = 0;
      const x = this.root;
      if (x === SENTINEL) {
        this.root = z;
        z.color = 0 /* Black */;
      } else if (node.right === SENTINEL) {
        node.right = z;
        z.parent = node;
      } else {
        const nextNode = leftest(node.right);
        nextNode.left = z;
        z.parent = nextNode;
      }
      fixInsert(this, z);
      return z;
    }
    /**
     *      node              node
     *     /  \              /  \
     *    a   b     ---->   a    b
     *                       \
     *                        z
     */
    rbInsertLeft(node, p) {
      const z = new TreeNode(p, 1 /* Red */);
      z.left = SENTINEL;
      z.right = SENTINEL;
      z.parent = SENTINEL;
      z.size_left = 0;
      z.lf_left = 0;
      if (this.root === SENTINEL) {
        this.root = z;
        z.color = 0 /* Black */;
      } else if (node.left === SENTINEL) {
        node.left = z;
        z.parent = node;
      } else {
        const prevNode = righttest(node.left);
        prevNode.right = z;
        z.parent = prevNode;
      }
      fixInsert(this, z);
      return z;
    }
    getContentOfSubTree(node) {
      let str = "";
      this.iterate(node, (node2) => {
        str += this.getNodeContent(node2);
        return true;
      });
      return str;
    }
    // #endregion
  };

  // codeoss-m4/stage/ports/codeoss/native-model.ts
  var nativeHost = globalThis.Coast;
  if (!nativeHost || nativeHost.protocol !== 1 || !isCoast || !isNative || isWeb || isElectron || platform2 !== "coast" || arch !== "x64" || OS !== 4 /* Coast */) {
    throw new Error("The native Coast host and explicit upstream platform adapter are required.");
  }
  var host = nativeHost;
  var runtime = { platform: PlatformToString(platform), arch, cwd: cwd(), node: false, electron: false, dom: false, fileWatching: false, eventLoop: false };
  function absolutePath(input) {
    if (!input || input.includes("\0")) {
      throw new Error("A nonempty file path is required.");
    }
    const path = input.startsWith("/") ? input : `${cwd()}/${input}`;
    const parts = [];
    for (const part of path.split("/")) {
      if (!part || part === ".") {
        continue;
      }
      if (part === "..") {
        parts.pop();
      } else {
        parts.push(part);
      }
    }
    return "/" + parts.join("/");
  }
  var NativeDocument = class _NativeDocument {
    tree;
    path;
    original;
    bom;
    savedContent;
    constructor(path, original, initial = null) {
      this.path = absolutePath(path);
      this.original = original;
      this.bom = (original ?? initial)?.startsWith("\uFEFF") ? "\uFEFF" : "";
      const content = (original ?? initial ?? "").slice(this.bom.length);
      const eol = content.includes("\r\n") ? "\r\n" : "\n";
      this.tree = new PieceTreeBase([new StringBuffer(content, createLineStartsFast(content))], eol, false);
      this.savedContent = original === null ? "" : content;
    }
    static open(path) {
      const p = absolutePath(path);
      return new _NativeDocument(p, host.readText(p));
    }
    static create(path) {
      return new _NativeDocument(path, null);
    }
    static createWithText(path, text) {
      return new _NativeDocument(path, null, text);
    }
    get hasBOM() {
      return this.bom.length !== 0;
    }
    get content() {
      return this.tree.getLinesRawContent();
    }
    get dirty() {
      return this.content !== this.savedContent;
    }
    get exists() {
      return this.original !== null;
    }
    boundary(at) {
      const content = this.content;
      if (!Number.isSafeInteger(at) || at < 0 || at > content.length) {
        throw new RangeError("Offset is outside the UTF-16 text.");
      }
      if (at && at < content.length && /[\uD800-\uDBFF]/.test(content[at - 1]) && /[\uDC00-\uDFFF]/.test(content[at])) {
        throw new RangeError("Offset splits a Unicode surrogate pair.");
      }
    }
    insert(at, value) {
      this.boundary(at);
      if (typeof value !== "string" || value.includes("\0") || /[\uD800-\uDBFF](?![\uDC00-\uDFFF])|(?<![\uD800-\uDBFF])[\uDC00-\uDFFF]/.test(value)) {
        throw new Error("Only valid Unicode text without NUL is supported.");
      }
      if (this.content.length + value.length > host.maxTextBytes) {
        throw new RangeError("Text exceeds the bounded model limit.");
      }
      this.tree.insert(at, value, false);
    }
    delete(at, count) {
      this.boundary(at);
      this.boundary(at + count);
      if (!Number.isSafeInteger(count) || count < 0) {
        throw new RangeError("Delete length is invalid.");
      }
      this.tree.delete(at, count);
    }
    snapshot() {
      const snapshot = this.tree.createSnapshot(this.bom);
      let result = "";
      for (let chunk = snapshot.read(); chunk !== null; chunk = snapshot.read()) {
        result += chunk;
      }
      return result;
    }
    save() {
      const value = this.snapshot();
      const result = host.saveText(this.path, value, this.original);
      this.original = value;
      this.savedContent = this.content;
      return result;
    }
    saveAs(path) {
      const target = absolutePath(path), value = this.snapshot();
      const result = host.saveText(target, value, null);
      this.path = target;
      this.original = value;
      this.savedContent = this.content;
      return result;
    }
  };

  // codeoss-m4/stage/ports/codeoss/graphical-model.ts
  var LIMIT = 256 * 1024;
  var CLIP = 65536;
  var HISTORY = 512 * 1024;
  function size(s) {
    let n = 0;
    for (const ch of s) {
      const c = ch.codePointAt(0);
      n += c < 128 ? 1 : c < 2048 ? 2 : c < 65536 ? 3 : 4;
    }
    return n;
  }
  function valid(s) {
    if (s.includes("\0") || /[\uD800-\uDBFF](?![\uDC00-\uDFFF])|(?<![\uD800-\uDBFF])[\uDC00-\uDFFF]/.test(s)) throw Error("Only valid Unicode text without NUL is supported.");
  }
  function bounded(n, max) {
    if (!Number.isSafeInteger(n) || n < 0 || n > max) throw RangeError("Position is outside the document.");
    return n;
  }
  function prev(s, n) {
    if (!n) return 0;
    if (n >= 2 && s.slice(n - 2, n) === "\r\n") return n - 2;
    n--;
    if (n && /[\uDC00-\uDFFF]/.test(s[n])) n--;
    return n;
  }
  function next(s, n) {
    if (n === s.length) return n;
    if (s.slice(n, n + 2) === "\r\n") return n + 2;
    return n + (s.codePointAt(n) > 65535 ? 2 : 1);
  }
  var GraphicalEditor = class {
    doc = NativeDocument.create("/home/user/Untitled.txt");
    cursor = 0;
    anchor = 0;
    revision = 1;
    tab = 4;
    spaces = true;
    preferred = -1;
    history = [];
    historyAt = 0;
    historyUnits = 0;
    message = "";
    selection = "";
    boundary(n) {
      const s = this.doc.content;
      bounded(n, s.length);
      if (n && s[n - 1] === "\r" && s[n] === "\n") throw RangeError("Position splits a CRLF line ending.");
      if (n && n < s.length && /[\uD800-\uDBFF]/.test(s[n - 1]) && /[\uDC00-\uDFFF]/.test(s[n])) throw RangeError("Position splits a Unicode scalar.");
    }
    reset(doc) {
      if (size(doc.snapshot()) > LIMIT) throw Error("Code OSS Core Preview supports files up to 256 KiB. The file is unchanged.");
      this.doc = doc;
      this.cursor = this.anchor = 0;
      this.history = [];
      this.historyAt = this.historyUnits = 0;
      this.revision++;
      this.preferred = -1;
    }
    edit(value) {
      valid(value);
      const text = this.doc.content, a = Math.min(this.cursor, this.anchor), b = Math.max(this.cursor, this.anchor);
      this.boundary(a);
      this.boundary(b);
      const result = text.slice(0, a) + value + text.slice(b);
      if (size(result) + (this.doc.hasBOM ? 3 : 0) > LIMIT) throw Error("The 256 KiB graphical preview limit would be exceeded. Existing text is preserved.");
      if (a === b && !value) return;
      const record = { at: a, removed: text.slice(a, b), inserted: value, cursor: this.cursor, anchor: this.anchor, after: a + value.length };
      while (this.history.length > this.historyAt) {
        const r = this.history.pop();
        this.historyUnits -= r.removed.length + r.inserted.length;
      }
      const units = record.removed.length + record.inserted.length;
      while (this.history.length && (this.history.length >= 64 || this.historyUnits + units > HISTORY)) {
        const r = this.history.shift();
        this.historyUnits -= r.removed.length + r.inserted.length;
        this.historyAt--;
      }
      if (b > a) this.doc.delete(a, b - a);
      if (value) this.doc.insert(a, value);
      this.history.push(record);
      this.historyUnits += units;
      this.historyAt = this.history.length;
      this.cursor = this.anchor = record.after;
      this.revision++;
      this.preferred = -1;
    }
    undo(redo) {
      if (redo ? this.historyAt === this.history.length : !this.historyAt) return;
      const r = this.history[redo ? this.historyAt : this.historyAt - 1], remove = redo ? r.removed : r.inserted, insert = redo ? r.inserted : r.removed;
      if (remove) this.doc.delete(r.at, remove.length);
      if (insert) this.doc.insert(r.at, insert);
      if (redo) {
        this.historyAt++;
        this.cursor = this.anchor = r.after;
      } else {
        this.historyAt--;
        this.cursor = r.cursor;
        this.anchor = r.anchor;
      }
      this.revision++;
      this.preferred = -1;
    }
    visual(line, at) {
      let col = 0;
      for (let i = 0; i < at; ) {
        const n = next(line, i);
        if (line[i] === "	") col += this.tab - col % this.tab;
        else if (line[i] !== "\r") col++;
        i = n;
      }
      return col;
    }
    column(line, wanted) {
      let col = 0, i = 0;
      while (i < line.length) {
        const n = next(line, i), width = line[i] === "	" ? this.tab - col % this.tab : line[i] === "\r" ? 0 : 1;
        if (width && wanted * 2 < col * 2 + width) break;
        col += width;
        i = n;
      }
      return i;
    }
    move(r) {
      const text = this.doc.content, p = this.doc.tree.getPositionAt(this.cursor);
      let n = this.cursor;
      if (r.a === 0 || r.a === 1) {
        n = r.a === 0 ? prev(text, n) : next(text, n);
        if (r.flags & 2) {
          while (n > 0 && n < text.length && /\s/.test(text[r.a === 0 ? n : prev(text, n)])) n = r.a === 0 ? prev(text, n) : next(text, n);
          while (n > 0 && n < text.length && !/\s/.test(text[r.a === 0 ? prev(text, n) : n])) n = r.a === 0 ? prev(text, n) : next(text, n);
        }
      } else if (r.a === 2) n = r.flags & 2 ? 0 : this.doc.tree.getOffsetAt(p.lineNumber, 1);
      else if (r.a === 3) n = r.flags & 2 ? text.length : this.doc.tree.getOffsetAt(p.lineNumber, this.doc.tree.getLineContent(p.lineNumber).length + 1);
      else if (r.a >= 4 && r.a <= 7) {
        const down = r.a === 5 || r.a === 7, amount = r.a >= 6 ? Math.max(1, Math.min(100, r.b)) : 1;
        if (this.preferred < 0) this.preferred = this.visual(this.doc.tree.getLineContent(p.lineNumber), p.column - 1);
        const line = Math.max(1, Math.min(this.doc.tree.getLineCount(), p.lineNumber + (down ? amount : -amount)));
        n = this.doc.tree.getOffsetAt(line, 1) + this.column(this.doc.tree.getLineContent(line), this.preferred);
      } else throw Error("Unsupported cursor movement.");
      if (r.a < 4) this.preferred = -1;
      this.boundary(n);
      this.cursor = n;
      if (!(r.flags & 1)) this.anchor = n;
    }
    dispatch(r) {
      this.message = "";
      this.selection = "";
      let error = 0;
      try {
        if (r.action === 0) {
          this.reset(r.text ? NativeDocument.open(r.text) : NativeDocument.create("/home/user/Untitled.txt"));
        } else if (r.action === 1) {
          let value = r.text;
          if (r.flags & 4) {
            const a = Math.min(this.cursor, this.anchor), p = this.doc.tree.getPositionAt(a), prefix = this.doc.tree.getLineContent(p.lineNumber).slice(0, p.column - 1).match(/^[ \t]*/)[0];
            value = this.doc.tree.getEOL() + prefix;
          } else if (r.flags & 8) {
            const p = this.doc.tree.getPositionAt(this.cursor), col = this.visual(this.doc.tree.getLineContent(p.lineNumber), p.column - 1);
            value = this.spaces ? " ".repeat(this.tab - col % this.tab) : "	";
          }
          this.edit(value);
        } else if (r.action === 2) this.move(r);
        else if (r.action === 3) {
          if (r.d && r.d !== this.revision) throw Error("The document changed before selection. Select it again.");
          if (this.cursor === this.anchor) {
            const text = this.doc.content;
            let n = r.a ? next(text, this.cursor) : prev(text, this.cursor);
            if (r.flags & 2) {
              while (n > 0 && n < text.length && /\s/.test(text[r.a ? prev(text, n) : n])) n = r.a ? next(text, n) : prev(text, n);
              while (n > 0 && n < text.length && !/\s/.test(text[r.a ? n : prev(text, n)])) n = r.a ? next(text, n) : prev(text, n);
            }
            this.anchor = n;
          }
          this.edit("");
        } else if (r.action === 4 || r.action === 5) this.undo(r.action === 5);
        else if (r.action === 6 || r.action === 7) {
          const result = r.action === 6 ? this.doc.save() : this.doc.saveAs(r.text);
          this.message = result.backupPath ? "Saved. Previous version retained: " + result.backupPath : "Saved.";
        } else if (r.action === 8) {
          if (r.d && r.d !== this.revision) throw Error("The document changed before the click. Click again.");
          this.boundary(r.a);
          this.boundary(r.b);
          this.cursor = r.a;
          this.anchor = r.b;
          this.preferred = -1;
        } else if (r.action === 9) {
          this.anchor = 0;
          this.cursor = this.doc.content.length;
          this.preferred = -1;
        } else if (r.action === 10) {
          const selected = this.doc.content.slice(Math.min(this.cursor, this.anchor), Math.max(this.cursor, this.anchor));
          if (size(selected) > CLIP) throw Error("Clipboard limit is 64 KiB. Select a smaller range.");
          this.selection = selected;
        } else if (r.action === 11) {
          valid(r.text);
          if (r.text) {
            const text = this.doc.content;
            let at = text.indexOf(r.text, this.cursor);
            if (at < 0) at = text.indexOf(r.text);
            if (at < 0) this.message = "No match. Search is literal and case-sensitive.";
            else {
              this.boundary(at);
              this.boundary(at + r.text.length);
              this.anchor = at;
              this.cursor = at + r.text.length;
              this.preferred = -1;
            }
          }
        } else if (r.action === 12) {
          if (![2, 4, 8].includes(r.a)) throw Error("Tab width must be 2, 4 or 8.");
          this.tab = r.a;
          this.spaces = !!r.b;
        } else if (r.action === 13) {
          valid(r.text);
          this.reset(NativeDocument.createWithText("/home/user/Recovered.txt", (r.flags & 16 ? "\uFEFF" : "") + r.text));
          this.cursor = this.anchor = Math.min(r.a, this.doc.content.length);
          this.boundary(this.cursor);
          this.message = "Recovered the last acknowledged text. Use Save As to create a new copy.";
        } else throw Error("Unsupported graphical action.");
      } catch (e) {
        error = 1;
        const v = e;
        this.message = Array.from((v.code ? v.code + ": " : "") + (v.message || String(e))).slice(0, 240).join("");
      }
      return this.snapshot(error);
    }
    snapshot(error = 0) {
      const p = this.doc.tree.getPositionAt(this.cursor), content = this.doc.content;
      return { error, path: this.doc.path, content, selection: this.selection, message: this.message, cursor: this.cursor, anchor: this.anchor, lines: this.doc.tree.getLineCount(), line: p.lineNumber - 1, column: this.visual(this.doc.tree.getLineContent(p.lineNumber), p.column - 1), revision: this.revision, eol: this.doc.tree.getEOL() === "\r\n" ? 2 : 1, tab: this.tab, flags: (this.doc.dirty || !this.doc.exists && this.doc.hasBOM ? 1 : 0) | (this.doc.exists ? 2 : 0) | (this.historyAt ? 4 : 0) | (this.historyAt < this.history.length ? 8 : 0) | (this.doc.hasBOM ? 16 : 0) | (this.spaces ? 32 : 0) };
    }
  };
  globalThis.codeOSSGraphical = new GraphicalEditor();
  var GraphicalService = class {
    documents = /* @__PURE__ */ new Map();
    get size() {
      return this.documents.size;
    }
    dispatch(r) {
      if (!Number.isSafeInteger(r.c) || r.c < 1 || r.c > 2147483647) throw Error("Invalid document identity.");
      let editor = this.documents.get(r.c);
      if (r.action === 0 || r.action === 13) {
        if (editor) throw Error("Duplicate document registration.");
        if (this.documents.size >= 32) throw Error("The graphical service document limit is 32.");
        editor = new GraphicalEditor();
        this.documents.set(r.c, editor);
      }
      if (!editor) throw Error("Unknown document identity.");
      if (r.action === 15) {
        const result = editor.snapshot();
        this.documents.delete(r.c);
        return result;
      }
      return editor.dispatch(r);
    }
  };
  globalThis.codeOSSService = new GraphicalService();
})();
