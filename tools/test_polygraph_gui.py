from __future__ import annotations

from tools.polygraph import build_parser
from tools.polygraph_gui import discover_command_specs, path_selector_kind, serialize_command


def _spec(key: str):
    specs = {spec.key: spec for spec in discover_command_specs(build_parser())}
    return specs[key]


def test_discovers_nested_commands_and_parent_arguments() -> None:
    specs = {spec.key: spec for spec in discover_command_specs(build_parser())}
    assert "gui" not in specs
    assert "stats ldbc" in specs
    assert "instance up" in specs
    assert [action.dest for action in specs["instance up"].actions] == [
        "scale_factor",
        "distribution",
        "services",
    ]


def test_serializes_instance_command_from_generated_fields() -> None:
    spec = _spec("instance restart")
    argv = serialize_command(
        spec,
        {
            "scale_factor": "sf0_1",
            "distribution": "hash-based",
            "recreate": True,
        },
    )
    assert argv == ["instance", "sf0_1", "hash-based", "restart", "--recreate"]


def test_serializes_required_file_option_and_passthrough() -> None:
    spec = _spec("stats ldbc")
    argv = serialize_command(
        spec,
        {"input": "/tmp/ldbc", "json_output": "/tmp/report.json"},
        "--sample 10",
    )
    assert argv == [
        "stats",
        "ldbc",
        "--input",
        "/tmp/ldbc",
        "--json-output",
        "/tmp/report.json",
        "--",
        "--sample",
        "10",
    ]


def test_path_selector_is_derived_from_argparse_metadata() -> None:
    spec = _spec("stats ldbc")
    actions = {action.dest: action for action in spec.actions}
    assert path_selector_kind(actions["input"]) == "directory"
    assert path_selector_kind(actions["json_output"]) == "save"


def test_invalid_passthrough_quoting_is_reported() -> None:
    spec = _spec("list")
    try:
        serialize_command(spec, {}, '"unfinished')
    except ValueError as exc:
        assert "No closing quotation" in str(exc)
    else:
        raise AssertionError("Expected invalid shell quoting to fail")
