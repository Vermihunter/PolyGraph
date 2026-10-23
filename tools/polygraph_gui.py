#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import queue
import re
import shlex
import signal
import subprocess
import sys
import threading
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.polygraph import build_parser

try:
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk
    from tkinter.scrolledtext import ScrolledText
except ImportError as exc:  # pragma: no cover - depends on host packages
    tk = None  # type: ignore[assignment]
    filedialog = messagebox = ttk = ScrolledText = None  # type: ignore[assignment]
    TK_IMPORT_ERROR: ImportError | None = exc
else:
    TK_IMPORT_ERROR = None


SETTINGS_FILE = Path(
    os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")
) / "polygraph" / "gui-settings.json"
ANSI_ESCAPE = re.compile(r"\x1B(?:[@-Z\\-_]|\[[0-?]*[ -/]*[@-~])")


@dataclass(frozen=True)
class CommandSpec:
    path: tuple[str, ...]
    parser: argparse.ArgumentParser
    action_groups: tuple[tuple[argparse.Action, ...], ...]
    help: str

    @property
    def actions(self) -> tuple[argparse.Action, ...]:
        return tuple(action for group in self.action_groups for action in group)

    @property
    def key(self) -> str:
        return " ".join(self.path)

    @property
    def title(self) -> str:
        return "  ›  ".join(self.path)


@dataclass
class FieldBinding:
    action: argparse.Action
    variable: tk.Variable
    widget: tk.Widget


def _is_subparsers(action: argparse.Action) -> bool:
    return isinstance(action, argparse._SubParsersAction)


def _visible_actions(parser: argparse.ArgumentParser) -> list[argparse.Action]:
    return [
        action
        for action in parser._actions
        if action.dest != "help"
        and action.help is not argparse.SUPPRESS
        and not _is_subparsers(action)
        and action.dest not in {"handler", "command"}
    ]


def _choice_help(action: argparse._SubParsersAction, name: str) -> str:
    for choice_action in action._choices_actions:
        if choice_action.dest == name:
            return choice_action.help or ""
    return ""


def discover_command_specs(
    parser: argparse.ArgumentParser,
    *,
    excluded_roots: Iterable[str] = ("gui",),
) -> list[CommandSpec]:
    excluded = set(excluded_roots)
    specs: list[CommandSpec] = []

    def visit(
        current: argparse.ArgumentParser,
        path: tuple[str, ...],
        action_groups: tuple[tuple[argparse.Action, ...], ...],
        inherited_help: str,
    ) -> None:
        subparser_actions = [action for action in current._actions if _is_subparsers(action)]
        if not subparser_actions:
            if path and path[0] not in excluded:
                description = current.description or inherited_help or current.prog
                specs.append(CommandSpec(path, current, action_groups, description.strip()))
            return

        for subparsers in subparser_actions:
            seen: set[int] = set()
            for name, child in subparsers.choices.items():
                # argparse aliases point to the same parser. Keep the first/canonical name only.
                parser_id = id(child)
                if parser_id in seen:
                    continue
                seen.add(parser_id)
                if not path and name in excluded:
                    continue
                child_help = _choice_help(subparsers, name)
                visit(
                    child,
                    (*path, name),
                    (*action_groups, tuple(_visible_actions(child))),
                    child_help,
                )

    visit(parser, (), (), parser.description or "")
    return specs


def _default_value(action: argparse.Action) -> Any:
    value = action.default
    if value is argparse.SUPPRESS or value is None:
        if isinstance(action, (argparse._StoreTrueAction, argparse._StoreFalseAction)):
            return False if isinstance(action, argparse._StoreTrueAction) else True
        return ""
    return value


def _stringify_value(value: Any) -> str:
    if value is None or value is argparse.SUPPRESS:
        return ""
    if isinstance(value, (list, tuple)):
        return ", ".join(str(item) for item in value)
    return str(value)


def _split_values(value: str) -> list[str]:
    value = value.strip()
    if not value:
        return []
    if "\n" in value or "," in value:
        return [item.strip() for item in re.split(r"[,\n]+", value) if item.strip()]
    return shlex.split(value)


def _preferred_option(action: argparse.Action) -> str:
    long_options = [option for option in action.option_strings if option.startswith("--")]
    return long_options[0] if long_options else action.option_strings[0]


def serialize_command(spec: CommandSpec, values: dict[str, Any], passthrough: str = "") -> list[str]:
    argv: list[str] = []

    def append_action(action: argparse.Action) -> None:
        is_optional = bool(action.option_strings)
        # Missing optional fields mean "let argparse use its CLI default".
        # Serializing parser defaults makes generated commands noisy and can
        # accidentally freeze defaults that should remain centrally defined.
        if is_optional and action.dest not in values:
            return
        value = values.get(action.dest, _default_value(action))

        if isinstance(action, argparse.BooleanOptionalAction):
            enabled = bool(value)
            default = bool(_default_value(action))
            if enabled == default:
                return
            positive = next((item for item in action.option_strings if not item.startswith("--no-")), None)
            negative = next((item for item in action.option_strings if item.startswith("--no-")), None)
            selected = positive if enabled else negative
            if selected:
                argv.append(selected)
            return

        if isinstance(action, argparse._StoreTrueAction):
            if bool(value):
                argv.append(_preferred_option(action))
            return
        if isinstance(action, argparse._StoreFalseAction):
            if not bool(value):
                argv.append(_preferred_option(action))
            return
        if isinstance(action, argparse._CountAction):
            count = int(value or 0)
            argv.extend([_preferred_option(action)] * count)
            return
        if isinstance(action, argparse._StoreConstAction):
            if bool(value):
                argv.append(_preferred_option(action))
            return

        rendered = _stringify_value(value).strip()
        if is_optional and not action.required:
            default_rendered = _stringify_value(_default_value(action)).strip()
            if rendered == default_rendered:
                return
        if isinstance(action, argparse._AppendAction):
            for item in _split_values(rendered):
                argv.extend([_preferred_option(action), item])
            return

        if action.nargs in ("*", "+", argparse.REMAINDER) or isinstance(action.nargs, int):
            items = _split_values(rendered)
            if not items:
                return
            if is_optional:
                argv.append(_preferred_option(action))
            argv.extend(items)
            return

        if not rendered:
            return
        if is_optional:
            argv.extend([_preferred_option(action), rendered])
        else:
            argv.append(rendered)

    for command_name, action_group in zip(spec.path, spec.action_groups):
        argv.append(command_name)
        for action in action_group:
            append_action(action)

    passthrough_tokens = shlex.split(passthrough) if passthrough.strip() else []
    if passthrough_tokens:
        argv.extend(["--", *passthrough_tokens])
    return argv


def required_missing(spec: CommandSpec, values: dict[str, Any]) -> list[str]:
    missing: list[str] = []
    for action in spec.actions:
        required = action.required or (
            not action.option_strings and action.nargs not in ("?", "*", argparse.REMAINDER)
        )
        if not required:
            continue
        value = values.get(action.dest, "")
        if isinstance(value, bool):
            continue
        if not _stringify_value(value).strip():
            missing.append(action.dest.replace("_", " "))
    return missing


def path_selector_kind(action: argparse.Action) -> str | None:
    if action.nargs in ("*", "+", argparse.REMAINDER) or isinstance(action, argparse._AppendAction):
        return None
    dest = action.dest.lower()
    help_text = (action.help or "").lower()
    combined = f"{dest} {help_text}"

    if "directory" in combined or dest.endswith("_dir") or dest in {"output_dir", "out_dir"}:
        return "directory"
    if dest in {"json_output"} or ("output" in dest and "path" in help_text):
        return "save"
    file_tokens = (
        "file",
        "config",
        "schema",
        "backends",
        "binary",
        "manifest",
        "dockerfile",
        "csv",
        "json",
    )
    if any(token in combined for token in file_tokens):
        return "file"
    return None


def load_settings() -> dict[str, Any]:
    try:
        raw = json.loads(SETTINGS_FILE.read_text(encoding="utf-8"))
        return raw if isinstance(raw, dict) else {}
    except (FileNotFoundError, json.JSONDecodeError, OSError):
        return {}


def save_settings(settings: dict[str, Any]) -> None:
    SETTINGS_FILE.parent.mkdir(parents=True, exist_ok=True)
    temporary = SETTINGS_FILE.with_suffix(".tmp")
    temporary.write_text(json.dumps(settings, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(SETTINGS_FILE)


class PolyGraphGui:
    BG = "#f4f6f8"
    SURFACE = "#ffffff"
    SIDEBAR = "#111827"
    SIDEBAR_ALT = "#1f2937"
    TEXT = "#111827"
    MUTED = "#6b7280"
    BORDER = "#dfe3e8"
    ACCENT = "#2563eb"
    ACCENT_ACTIVE = "#1d4ed8"
    SUCCESS = "#059669"
    DANGER = "#dc2626"
    CONSOLE = "#0b1220"
    CONSOLE_TEXT = "#dbeafe"

    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("PolyGraph Control Center")
        self.root.minsize(980, 720)
        self._center_window(1180, 860)
        self.root.configure(background=self.BG)

        self.parser = build_parser()
        self.specs = discover_command_specs(self.parser)
        self.spec_by_key = {spec.key: spec for spec in self.specs}
        self.settings = load_settings()
        self.bindings: dict[str, FieldBinding] = {}
        self.current_spec: CommandSpec | None = None
        self.process: subprocess.Popen[str] | None = None
        self.output_queue: queue.Queue[str | None] = queue.Queue()
        self.command_values: dict[str, dict[str, Any]] = dict(self.settings.get("commands", {}))
        self.last_directory = Path(self.settings.get("lastDirectory", ROOT))
        if not self.last_directory.is_dir():
            self.last_directory = ROOT

        self.search_var = tk.StringVar()
        self.preview_var = tk.StringVar()
        self.passthrough_var = tk.StringVar()
        self.status_var = tk.StringVar(value="Ready")
        self._configure_styles()
        self._build_layout()
        self._populate_commands()
        self._select_initial_command()
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)
        self.root.after(80, self._drain_output_queue)

    def _center_window(self, width: int, height: int) -> None:
        screen_width = self.root.winfo_screenwidth()
        screen_height = self.root.winfo_screenheight()
        x = max((screen_width - width) // 2, 0)
        y = max((screen_height - height) // 2, 0)
        self.root.geometry(f"{width}x{height}+{x}+{y}")

    def _configure_styles(self) -> None:
        style = ttk.Style(self.root)
        preferred = "clam" if "clam" in style.theme_names() else style.theme_use()
        style.theme_use(preferred)
        style.configure("App.TFrame", background=self.BG)
        style.configure("Surface.TFrame", background=self.SURFACE)
        style.configure("Sidebar.TFrame", background=self.SIDEBAR)
        style.configure(
            "Title.TLabel",
            background=self.BG,
            foreground=self.TEXT,
            font=("TkDefaultFont", 20, "bold"),
        )
        style.configure(
            "Subtitle.TLabel",
            background=self.BG,
            foreground=self.MUTED,
            font=("TkDefaultFont", 10),
        )
        style.configure(
            "CommandTitle.TLabel",
            background=self.SURFACE,
            foreground=self.TEXT,
            font=("TkDefaultFont", 16, "bold"),
        )
        style.configure(
            "Field.TLabel",
            background=self.SURFACE,
            foreground=self.TEXT,
            font=("TkDefaultFont", 10, "bold"),
        )
        style.configure(
            "Help.TLabel",
            background=self.SURFACE,
            foreground=self.MUTED,
            font=("TkDefaultFont", 9),
        )
        style.configure(
            "Status.TLabel",
            background=self.BG,
            foreground=self.SUCCESS,
            font=("TkDefaultFont", 10, "bold"),
        )
        style.configure("Card.TLabelframe", background=self.SURFACE, borderwidth=1, relief="solid")
        style.configure("Card.TLabelframe.Label", background=self.SURFACE, foreground=self.TEXT)
        style.configure("TEntry", padding=(8, 7))
        style.configure("TCombobox", padding=(8, 7))
        style.configure("TCheckbutton", background=self.SURFACE, foreground=self.TEXT)
        style.configure("Secondary.TButton", padding=(12, 8))
        style.configure(
            "Accent.TButton",
            background=self.ACCENT,
            foreground="#ffffff",
            padding=(16, 9),
            borderwidth=0,
            font=("TkDefaultFont", 10, "bold"),
        )
        style.map(
            "Accent.TButton",
            background=[("active", self.ACCENT_ACTIVE), ("disabled", "#93c5fd")],
            foreground=[("disabled", "#eff6ff")],
        )
        style.configure(
            "Danger.TButton",
            background=self.DANGER,
            foreground="#ffffff",
            padding=(12, 9),
            borderwidth=0,
        )
        style.map("Danger.TButton", background=[("active", "#b91c1c")])
        style.configure(
            "Commands.Treeview",
            background=self.SIDEBAR,
            fieldbackground=self.SIDEBAR,
            foreground="#d1d5db",
            borderwidth=0,
            rowheight=30,
            font=("TkDefaultFont", 10),
        )
        style.map(
            "Commands.Treeview",
            background=[("selected", self.ACCENT)],
            foreground=[("selected", "#ffffff")],
        )

    def _build_layout(self) -> None:
        app = ttk.Frame(self.root, style="App.TFrame", padding=(22, 18, 22, 20))
        app.pack(fill="both", expand=True)

        header = ttk.Frame(app, style="App.TFrame")
        header.pack(fill="x", pady=(0, 16))
        ttk.Label(header, text="PolyGraph Control Center", style="Title.TLabel").pack(side="left")
        ttk.Label(header, textvariable=self.status_var, style="Status.TLabel").pack(side="right", pady=(8, 0))
        ttk.Label(
            app,
            text="A generated interface for every command and option exposed by tools/polygraph.py.",
            style="Subtitle.TLabel",
        ).pack(anchor="w", pady=(0, 14))

        body = ttk.Panedwindow(app, orient="horizontal")
        body.pack(fill="both", expand=True)

        sidebar = ttk.Frame(body, style="Sidebar.TFrame", padding=(14, 16))
        content = ttk.Frame(body, style="Surface.TFrame", padding=(22, 18))
        body.add(sidebar, weight=1)
        body.add(content, weight=4)

        tk.Label(
            sidebar,
            text="COMMANDS",
            background=self.SIDEBAR,
            foreground="#9ca3af",
            font=("TkDefaultFont", 9, "bold"),
        ).pack(anchor="w", pady=(0, 8))
        search = tk.Entry(
            sidebar,
            textvariable=self.search_var,
            background=self.SIDEBAR_ALT,
            foreground="#f9fafb",
            insertbackground="#f9fafb",
            relief="flat",
            highlightthickness=1,
            highlightbackground="#374151",
            highlightcolor=self.ACCENT,
            font=("TkDefaultFont", 10),
        )
        search.pack(fill="x", ipady=7, pady=(0, 12))
        self.search_var.trace_add("write", lambda *_: self._populate_commands())

        self.command_tree = ttk.Treeview(
            sidebar,
            show="tree",
            selectmode="browse",
            style="Commands.Treeview",
        )
        self.command_tree.pack(fill="both", expand=True)
        self.command_tree.bind("<<TreeviewSelect>>", self._on_command_selected)

        content.columnconfigure(0, weight=1)
        content.rowconfigure(3, weight=5, minsize=280)
        content.rowconfigure(6, weight=2, minsize=90)

        self.command_title = ttk.Label(content, text="", style="CommandTitle.TLabel")
        self.command_title.grid(row=0, column=0, sticky="w")
        self.command_help = ttk.Label(
            content,
            text="",
            style="Help.TLabel",
            wraplength=780,
            justify="left",
        )
        self.command_help.grid(row=1, column=0, sticky="ew", pady=(5, 14))
        ttk.Separator(content).grid(row=2, column=0, sticky="ew", pady=(0, 12))

        form_shell = ttk.Frame(content, style="Surface.TFrame")
        form_shell.grid(row=3, column=0, sticky="nsew")
        self.form_canvas = tk.Canvas(
            form_shell,
            background=self.SURFACE,
            highlightthickness=0,
            borderwidth=0,
        )
        form_scroll = ttk.Scrollbar(form_shell, orient="vertical", command=self.form_canvas.yview)
        self.form_canvas.configure(yscrollcommand=form_scroll.set)
        form_scroll.pack(side="right", fill="y")
        self.form_canvas.pack(side="left", fill="both", expand=True)
        self.form_frame = ttk.Frame(self.form_canvas, style="Surface.TFrame")
        self.form_window = self.form_canvas.create_window((0, 0), window=self.form_frame, anchor="nw")
        self.form_frame.bind("<Configure>", self._update_form_scrollregion)
        self.form_canvas.bind("<Configure>", self._resize_form_window)
        self.form_canvas.bind_all("<MouseWheel>", self._on_mousewheel)

        preview_card = ttk.LabelFrame(content, text=" Command preview ", style="Card.TLabelframe", padding=10)
        preview_card.grid(row=4, column=0, sticky="ew", pady=(12, 8))
        preview_row = ttk.Frame(preview_card, style="Surface.TFrame")
        preview_row.pack(fill="x")
        preview = ttk.Entry(preview_row, textvariable=self.preview_var, state="readonly")
        preview.pack(side="left", fill="x", expand=True)
        ttk.Button(preview_row, text="Copy", style="Secondary.TButton", command=self._copy_preview).pack(
            side="left", padx=(8, 0)
        )

        passthrough_row = ttk.Frame(preview_card, style="Surface.TFrame")
        passthrough_row.pack(fill="x", pady=(8, 0))
        ttk.Label(passthrough_row, text="Arguments after --", style="Help.TLabel").pack(side="left")
        passthrough = ttk.Entry(passthrough_row, textvariable=self.passthrough_var)
        passthrough.pack(side="left", fill="x", expand=True, padx=(10, 0))
        self.passthrough_var.trace_add("write", lambda *_: self._refresh_preview())

        actions = ttk.Frame(content, style="Surface.TFrame")
        actions.grid(row=5, column=0, sticky="ew", pady=(4, 8))
        self.run_button = ttk.Button(actions, text="Run command", style="Accent.TButton", command=self._run)
        self.run_button.pack(side="left")
        self.stop_button = ttk.Button(
            actions,
            text="Stop",
            style="Danger.TButton",
            command=self._stop,
            state="disabled",
        )
        self.stop_button.pack(side="left", padx=(8, 0))
        ttk.Button(actions, text="Reset values", style="Secondary.TButton", command=self._reset_values).pack(
            side="right"
        )

        console_card = ttk.LabelFrame(content, text=" Output ", style="Card.TLabelframe", padding=0)
        console_card.grid(row=6, column=0, sticky="nsew")
        self.console = ScrolledText(
            console_card,
            height=10,
            background=self.CONSOLE,
            foreground=self.CONSOLE_TEXT,
            insertbackground=self.CONSOLE_TEXT,
            selectbackground="#1d4ed8",
            relief="flat",
            borderwidth=0,
            font=("TkFixedFont", 10),
            padx=12,
            pady=10,
            wrap="word",
            state="disabled",
        )
        self.console.pack(fill="both", expand=True)

    def _update_form_scrollregion(self, _event: tk.Event[Any]) -> None:
        self.form_canvas.configure(scrollregion=self.form_canvas.bbox("all"))

    def _resize_form_window(self, event: tk.Event[Any]) -> None:
        self.form_canvas.itemconfigure(self.form_window, width=event.width)

    def _on_mousewheel(self, event: tk.Event[Any]) -> None:
        if self.form_canvas.winfo_containing(event.x_root, event.y_root) is not None:
            self.form_canvas.yview_scroll(int(-1 * (event.delta / 120)), "units")

    def _populate_commands(self) -> None:
        selected_key = self.current_spec.key if self.current_spec else None
        query = self.search_var.get().strip().lower()
        self.command_tree.delete(*self.command_tree.get_children())
        nodes: dict[tuple[str, ...], str] = {}
        for spec in self.specs:
            searchable = f"{spec.key} {spec.help}".lower()
            if query and query not in searchable:
                continue
            parent = ""
            for depth, segment in enumerate(spec.path):
                prefix = spec.path[: depth + 1]
                if prefix not in nodes:
                    is_leaf = depth == len(spec.path) - 1
                    item_id = self.command_tree.insert(
                        parent,
                        "end",
                        text=segment,
                        open=True,
                        values=(spec.key if is_leaf else "",),
                    )
                    nodes[prefix] = item_id
                parent = nodes[prefix]
        if selected_key:
            self._select_tree_key(selected_key)

    def _select_tree_key(self, key: str) -> bool:
        def walk(parent: str = "") -> bool:
            for item in self.command_tree.get_children(parent):
                values = self.command_tree.item(item, "values")
                if values and values[0] == key:
                    self.command_tree.selection_set(item)
                    self.command_tree.focus(item)
                    self.command_tree.see(item)
                    return True
                if walk(item):
                    return True
            return False

        return walk()

    def _select_initial_command(self) -> None:
        requested = str(self.settings.get("lastCommand", ""))
        if requested in self.spec_by_key and self._select_tree_key(requested):
            return
        if self.specs:
            self._select_tree_key(self.specs[0].key)

    def _on_command_selected(self, _event: tk.Event[Any]) -> None:
        selection = self.command_tree.selection()
        if not selection:
            return
        values = self.command_tree.item(selection[0], "values")
        if not values or not values[0]:
            return
        spec = self.spec_by_key.get(values[0])
        if spec and spec is not self.current_spec:
            self._capture_current_values()
            self.current_spec = spec
            self._render_form(spec)

    def _render_form(self, spec: CommandSpec) -> None:
        for child in self.form_frame.winfo_children():
            child.destroy()
        self.bindings.clear()
        self.command_title.configure(text=spec.title)
        self.command_help.configure(text=spec.help)

        saved = self.command_values.get(spec.key, {})
        saved_values = saved.get("values", {}) if isinstance(saved, dict) else {}
        self.passthrough_var.set(saved.get("passthrough", "") if isinstance(saved, dict) else "")

        if not spec.actions:
            ttk.Label(
                self.form_frame,
                text="This command has no configurable arguments.",
                style="Help.TLabel",
            ).pack(anchor="w", pady=12)

        for action in spec.actions:
            self._add_action_field(action, saved_values)

        self.form_canvas.yview_moveto(0)
        self._refresh_preview()

    def _add_action_field(self, action: argparse.Action, saved_values: dict[str, Any]) -> None:
        default = saved_values.get(action.dest, _default_value(action))
        choices = list(action.choices) if action.choices is not None else []
        required = action.required or (
            not action.option_strings and action.nargs not in ("?", "*", argparse.REMAINDER)
        )
        label_text = action.dest.replace("_", " ").title()
        if action.option_strings:
            label_text += f"   {' / '.join(action.option_strings)}"
        if required:
            label_text += "  *"

        field = ttk.Frame(self.form_frame, style="Surface.TFrame")
        field.pack(fill="x", pady=(0, 12))
        ttk.Label(field, text=label_text, style="Field.TLabel").pack(anchor="w", pady=(0, 5))

        if isinstance(
            action,
            (
                argparse._StoreTrueAction,
                argparse._StoreFalseAction,
                argparse.BooleanOptionalAction,
                argparse._StoreConstAction,
            ),
        ):
            variable = tk.BooleanVar(value=bool(default))
            widget = ttk.Checkbutton(field, variable=variable, text="Enabled")
            widget.pack(anchor="w")
        else:
            variable = tk.StringVar(value=_stringify_value(default))
            row = ttk.Frame(field, style="Surface.TFrame")
            row.pack(fill="x")
            if choices and action.nargs not in ("*", "+") and not isinstance(action, argparse._AppendAction):
                if not variable.get() and choices:
                    variable.set(str(choices[0]))
                widget = ttk.Combobox(
                    row,
                    textvariable=variable,
                    values=[str(item) for item in choices],
                    state="readonly",
                )
            else:
                widget = ttk.Entry(row, textvariable=variable)
            widget.pack(side="left", fill="x", expand=True)

            selector = None if choices else path_selector_kind(action)
            if selector:
                ttk.Button(
                    row,
                    text="Browse…",
                    style="Secondary.TButton",
                    command=lambda a=action, v=variable, k=selector: self._browse(a, v, k),
                ).pack(side="left", padx=(8, 0))

        help_parts: list[str] = []
        if action.help:
            help_parts.append(action.help)
        if action.nargs in ("*", "+", argparse.REMAINDER) or isinstance(action, argparse._AppendAction):
            help_parts.append("Enter multiple values separated by commas, spaces, or new lines.")
        if choices and (action.nargs in ("*", "+") or isinstance(action, argparse._AppendAction)):
            help_parts.append("Choices: " + ", ".join(str(item) for item in choices))
        if help_parts:
            ttk.Label(
                field,
                text="  ".join(help_parts),
                style="Help.TLabel",
                wraplength=760,
                justify="left",
            ).pack(anchor="w", fill="x", pady=(5, 0))

        variable.trace_add("write", lambda *_: self._refresh_preview())
        self.bindings[action.dest] = FieldBinding(action, variable, widget)

    def _browse(self, action: argparse.Action, variable: tk.StringVar, kind: str) -> None:
        initial_dir = self.last_directory if self.last_directory.is_dir() else ROOT
        selected = ""
        if kind == "directory":
            selected = filedialog.askdirectory(initialdir=initial_dir, title=f"Select {action.dest}")
        elif kind == "save":
            selected = filedialog.asksaveasfilename(initialdir=initial_dir, title=f"Select {action.dest}")
        else:
            selected = filedialog.askopenfilename(initialdir=initial_dir, title=f"Select {action.dest}")
        if not selected:
            return
        path = Path(selected)
        self.last_directory = path if path.is_dir() else path.parent
        variable.set(str(path))

    def _values(self) -> dict[str, Any]:
        return {dest: binding.variable.get() for dest, binding in self.bindings.items()}

    def _capture_current_values(self) -> None:
        if not self.current_spec:
            return
        self.command_values[self.current_spec.key] = {
            "values": self._values(),
            "passthrough": self.passthrough_var.get(),
        }

    def _command_argv(self) -> list[str]:
        if not self.current_spec:
            return []
        return serialize_command(self.current_spec, self._values(), self.passthrough_var.get())

    def _display_command(self) -> str:
        argv = [sys.executable, str(ROOT / "tools" / "polygraph.py"), *self._command_argv()]
        try:
            return shlex.join(argv)
        except AttributeError:  # pragma: no cover - Python < 3.8
            return " ".join(shlex.quote(item) for item in argv)

    def _refresh_preview(self) -> None:
        if not self.current_spec:
            self.preview_var.set("")
            return
        try:
            self.preview_var.set(self._display_command())
        except ValueError as exc:
            self.preview_var.set(f"Invalid argument quoting: {exc}")

    def _copy_preview(self) -> None:
        text = self.preview_var.get()
        self.root.clipboard_clear()
        self.root.clipboard_append(text)
        self.status_var.set("Copied")
        self.root.after(1400, lambda: self.status_var.set("Ready") if not self.process else None)

    def _reset_values(self) -> None:
        if not self.current_spec:
            return
        self.command_values.pop(self.current_spec.key, None)
        self._render_form(self.current_spec)
        self.status_var.set("Defaults restored")

    def _persist(self) -> None:
        self._capture_current_values()
        payload = {
            "version": 1,
            "lastCommand": self.current_spec.key if self.current_spec else "",
            "lastDirectory": str(self.last_directory),
            "commands": self.command_values,
        }
        save_settings(payload)

    def _append_console(self, text: str) -> None:
        cleaned = ANSI_ESCAPE.sub("", text)
        self.console.configure(state="normal")
        self.console.insert("end", cleaned)
        self.console.see("end")
        self.console.configure(state="disabled")

    def _clear_console(self) -> None:
        self.console.configure(state="normal")
        self.console.delete("1.0", "end")
        self.console.configure(state="disabled")

    def _run(self) -> None:
        if self.process or not self.current_spec:
            return
        values = self._values()
        missing = required_missing(self.current_spec, values)
        if missing:
            messagebox.showerror("Missing required values", "Please fill in: " + ", ".join(missing))
            return

        try:
            command_argv = self._command_argv()
        except ValueError as exc:
            messagebox.showerror("Invalid arguments", str(exc))
            return

        self._persist()
        command = [sys.executable, str(ROOT / "tools" / "polygraph.py"), *command_argv]
        self._clear_console()
        self._append_console("$ " + self._display_command() + "\n\n")
        self.status_var.set("Running")
        self.run_button.configure(state="disabled")
        self.stop_button.configure(state="normal")

        try:
            self.process = subprocess.Popen(
                command,
                cwd=ROOT,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                bufsize=1,
                start_new_session=os.name != "nt",
            )
        except OSError as exc:
            self.process = None
            self._append_console(f"error: {exc}\n")
            self._finish_process(1)
            return

        threading.Thread(target=self._read_process_output, daemon=True).start()

    def _read_process_output(self) -> None:
        assert self.process is not None
        process = self.process
        if process.stdout is not None:
            for line in process.stdout:
                self.output_queue.put(line)
        return_code = process.wait()
        self.output_queue.put(f"\n[process exited with code {return_code}]\n")
        self.output_queue.put(None)

    def _drain_output_queue(self) -> None:
        try:
            while True:
                item = self.output_queue.get_nowait()
                if item is None:
                    return_code = self.process.returncode if self.process else 0
                    self._finish_process(return_code or 0)
                else:
                    self._append_console(item)
        except queue.Empty:
            pass
        finally:
            self.root.after(80, self._drain_output_queue)

    def _finish_process(self, return_code: int) -> None:
        self.process = None
        self.run_button.configure(state="normal")
        self.stop_button.configure(state="disabled")
        self.status_var.set("Completed" if return_code == 0 else f"Failed ({return_code})")

    def _terminate_process(self, process: subprocess.Popen[str]) -> None:
        if process.poll() is not None:
            return
        if os.name != "nt":
            os.killpg(process.pid, signal.SIGTERM)
        else:  # pragma: no cover - Windows-specific fallback
            process.terminate()
        self.root.after(2000, lambda: self._force_kill(process))

    @staticmethod
    def _force_kill(process: subprocess.Popen[str]) -> None:
        if process.poll() is not None:
            return
        if os.name != "nt":
            os.killpg(process.pid, signal.SIGKILL)
        else:  # pragma: no cover - Windows-specific fallback
            process.kill()

    def _stop(self) -> None:
        if not self.process:
            return
        self.status_var.set("Stopping")
        self._terminate_process(self.process)

    def _on_close(self) -> None:
        self._persist()
        if self.process and self.process.poll() is None:
            if not messagebox.askyesno("Command is running", "Stop the running command and close?"):
                return
            self._terminate_process(self.process)
        self.root.destroy()


def main() -> int:
    if tk is None:
        raise RuntimeError(
            "Tkinter is required for the GUI. On Ubuntu install it with: "
            "sudo apt install python3-tk"
        ) from TK_IMPORT_ERROR
    root = tk.Tk()
    PolyGraphGui(root)
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
