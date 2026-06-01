"""Resolver exception hierarchy for pure-parameterization refactor."""


class SpecResolveError(Exception):
    """Base class for all elaborator helper failures."""


class ExprSyntaxError(SpecResolveError):
    """ast.parse failed on a width_param expression."""


class ExprNameError(SpecResolveError):
    """A symbol in width_param was not found in any namespace
    (field_widths, port_parameters, derived totals)."""


class ExprNotAllowedError(SpecResolveError):
    """width_param contains a forbidden ast node
    (function call, attribute access, subscript, comprehension, lambda, ...)."""


class FieldNotFoundError(SpecResolveError):
    """A field name passed to a helper does not exist in the spec."""
