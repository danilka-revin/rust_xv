import { basicSetup } from 'codemirror';
import { EditorView, keymap, Decoration, type DecorationSet } from '@codemirror/view';
import { EditorState, StateEffect, StateField, RangeSetBuilder, type Extension } from '@codemirror/state';
import { cpp } from '@codemirror/lang-cpp';
import { indentWithTab } from '@codemirror/commands';
import { defaultHighlightStyle, syntaxHighlighting, HighlightStyle } from '@codemirror/language';
import { tags } from '@lezer/highlight';

export interface DiagnosticMarker {
  line: number;
  severity: 'error' | 'warning' | 'note' | 'info';
  message: string;
}

const setDiagnostics = StateEffect.define<DiagnosticMarker[]>();

const diagnosticField = StateField.define<DecorationSet>({
  create: () => Decoration.none,
  update(decorations, tr) {
    decorations = decorations.map(tr.changes);
    for (const effect of tr.effects) {
      if (!effect.is(setDiagnostics)) continue;
      const builder = new RangeSetBuilder<Decoration>();
      const doc = tr.state.doc;
      const sorted = [...effect.value].filter((d) => d.line > 0).sort((a, b) => a.line - b.line);
      for (const marker of sorted) {
        if (marker.line > doc.lines) continue;
        const line = doc.line(marker.line);
        builder.add(
          line.from,
          line.from,
          Decoration.line({
            class: `cm-diagnostic cm-diagnostic-${marker.severity}`,
            attributes: { title: marker.message },
          }),
        );
      }
      decorations = builder.finish();
    }
    return decorations;
  },
  provide: (field) => EditorView.decorations.from(field),
});

/** opencode-flavoured syntax colours: warm peach functions, violet keywords, mint strings. */
const opencodeHighlight = HighlightStyle.define([
  { tag: [tags.keyword, tags.controlKeyword, tags.operatorKeyword, tags.modifier, tags.self], color: '#9d7cd8' },
  { tag: [tags.function(tags.variableName), tags.function(tags.propertyName)], color: '#fab283' },
  { tag: tags.variableName, color: '#eeeeee' },
  { tag: tags.propertyName, color: '#e5c07b' },
  { tag: [tags.typeName, tags.className, tags.namespace, tags.tagName], color: '#e5c07b' },
  { tag: [tags.string, tags.special(tags.string)], color: '#7fd88f' },
  { tag: [tags.number, tags.bool, tags.null, tags.atom], color: '#f5a742' },
  { tag: [tags.operator, tags.compareOperator, tags.logicOperator, tags.arithmeticOperator], color: '#56b6c2' },
  { tag: [tags.punctuation, tags.bracket, tags.separator], color: '#c9c9c9' },
  { tag: tags.comment, color: '#7d7d7d', fontStyle: 'italic' },
  { tag: tags.meta, color: '#56b6c2' },
  { tag: tags.processingInstruction, color: '#c792ea' },
  { tag: tags.invalid, color: '#e06c75', textDecoration: 'underline wavy' },
]);

const opencodeTheme = EditorView.theme(
  {
    '&': { color: '#eeeeee', backgroundColor: 'transparent', height: '100%', fontSize: '13.5px' },
    '.cm-scroller': { fontFamily: 'var(--mono)', lineHeight: '1.65', overflow: 'auto' },
    '.cm-content': { caretColor: '#fab283', padding: '16px 0 45vh' },
    '.cm-cursor, .cm-dropCursor': { borderLeftColor: '#fab283', borderLeftWidth: '2px' },
    '.cm-selectionBackground, &.cm-focused .cm-selectionBackground, ::selection': { backgroundColor: '#2c3441' },
    '.cm-activeLine': { backgroundColor: '#ffffff06' },
    '.cm-gutters': { backgroundColor: 'transparent', color: '#4b4b4b', border: 'none', paddingLeft: '10px' },
    '.cm-activeLineGutter': { backgroundColor: 'transparent', color: '#9a9a9a' },
    '.cm-lineNumbers .cm-gutterElement': { minWidth: '34px', padding: '0 8px 0 6px' },
    '.cm-matchingBracket, .cm-nonmatchingBracket': { backgroundColor: '#fab28322', outline: '1px solid #fab28355' },
    '.cm-tooltip': { backgroundColor: '#141414', border: '1px solid #303030', borderRadius: '8px', color: '#eeeeee' },
    '.cm-panels': { backgroundColor: '#141414', color: '#eeeeee' },
    '.cm-searchMatch': { backgroundColor: '#fab28333', outline: '1px solid #fab28355' },
    '.cm-searchMatch.cm-searchMatch-selected': { backgroundColor: '#fab28355' },
    '.cm-diagnostic': { borderLeft: '2px solid transparent' },
    '.cm-diagnostic-error': { backgroundColor: '#e06c7514', borderLeftColor: '#e06c75' },
    '.cm-diagnostic-warning': { backgroundColor: '#f5a74214', borderLeftColor: '#f5a742' },
    '.cm-diagnostic-info, .cm-diagnostic-note': { backgroundColor: '#56b6c214', borderLeftColor: '#56b6c2' },
    '&.cm-focused': { outline: 'none' },
  },
  { dark: true },
);

export interface SketchEditor {
  view: EditorView;
  getValue(): string;
  setValue(value: string): void;
  setDiagnostics(diagnostics: DiagnosticMarker[]): void;
  focusLine(line: number): void;
  destroy(): void;
}

export function createEditor(parent: HTMLElement, initial: string, onChange: () => void): SketchEditor {
  const extensions: Extension[] = [
    basicSetup,
    cpp(),
    keymap.of([indentWithTab]),
    syntaxHighlighting(opencodeHighlight),
    syntaxHighlighting(defaultHighlightStyle, { fallback: true }),
    opencodeTheme,
    diagnosticField,
    EditorView.updateListener.of((update) => {
      if (update.docChanged) onChange();
    }),
  ];

  const view = new EditorView({
    parent,
    state: EditorState.create({ doc: initial, extensions }),
  });

  return {
    view,
    getValue: () => view.state.doc.toString(),
    setValue(value) {
      if (view.state.doc.toString() === value) return;
      // A fresh state drops undo history, so Ctrl+Z cannot resurrect the
      // previously open file.
      view.setState(EditorState.create({ doc: value, extensions }));
    },
    setDiagnostics(diagnostics) {
      view.dispatch({ effects: setDiagnostics.of(diagnostics) });
    },
    focusLine(line) {
      if (line <= 0) return;
      const target = view.state.doc.line(Math.min(line, view.state.doc.lines));
      view.dispatch({
        selection: { anchor: target.from },
        effects: EditorView.scrollIntoView(target.from, { y: 'center' }),
      });
      view.focus();
    },
    destroy() {
      view.destroy();
    },
  };
}
