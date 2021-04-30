import weakref
from typing import List, Generator, Iterable, Tuple, Union, Set, Optional, Dict
import logging
from collections import defaultdict

import claripy
from claripy.annotation import Annotation
from archinfo import Arch
from ailment.expression import BinaryOp, StackBaseOffset

from ...engines.light import SpOffset
from ...sim_variable import SimVariable
from ...storage.memory_mixins import MultiValuedMemory
from ..analysis import Analysis
from ..typehoon.typevars import TypeVariables

l = logging.getLogger(name=__name__)


def parse_stack_pointer(sp):
    """
    Convert multiple supported forms of stack pointer representations into stack offsets.

    :param sp:  A stack pointer representation.
    :return:    A stack pointer offset.
    :rtype:     int
    """
    if isinstance(sp, int):
        return sp

    if isinstance(sp, StackBaseOffset):
        return sp.offset

    if isinstance(sp, BinaryOp):
        op0, op1 = sp.operands
        off0 = parse_stack_pointer(op0)
        off1 = parse_stack_pointer(op1)
        if sp.op == "Sub":
            return off0 - off1
        elif sp.op == "Add":
            return off0 + off1

    raise NotImplementedError("Unsupported stack pointer representation type %s." % type(sp))


class VariableAnnotation(Annotation):

    __slots__ = ('addr_and_variables', )

    def __init__(self, addr_and_variables: List[Tuple[int,SimVariable]]):
        self.addr_and_variables = addr_and_variables

    @property
    def relocatable(self):
        return True

    @property
    def eliminatable(self):
        return False


class VariableRecoveryBase(Analysis):
    """
    The base class for VariableRecovery and VariableRecoveryFast.
    """

    def __init__(self, func, max_iterations):

        self.function = func
        self.variable_manager = self.kb.variables

        self._max_iterations = max_iterations

        self._outstates = {}
        self._instates = {}
        self._dominance_frontiers = None

    #
    # Public methods
    #

    def get_variable_definitions(self, block_addr):
        """
        Get variables that are defined at the specified block.

        :param int block_addr:  Address of the block.
        :return:                A set of variables.
        """

        if block_addr in self._outstates:
            return self._outstates[block_addr].variables
        return set()

    #
    # Private methods
    #

    def initialize_dominance_frontiers(self):
        # Computer the dominance frontier for each node in the graph
        df = self.project.analyses.DominanceFrontier(self.function)
        self._dominance_frontiers = defaultdict(set)
        for b0, domfront in df.frontiers.items():
            for d in domfront:
                self._dominance_frontiers[d.addr].add(b0.addr)


class VariableRecoveryStateBase:
    """
    The base abstract state for variable recovery analysis.
    """

    _tops = {}

    def __init__(self, block_addr, analysis, arch, func, stack_region=None, register_region=None, global_region=None,
                 typevars=None, type_constraints=None, delayed_type_constraints=None, project=None):

        self.block_addr = block_addr
        self._analysis = analysis
        self.arch: Arch = arch
        self.function = func
        self.project = project

        if stack_region is not None:
            self.stack_region: MultiValuedMemory = stack_region
            self.stack_region._phi_maker = self._make_phi_variable
        else:
            self.stack_region: MultiValuedMemory = MultiValuedMemory(memory_id="mem", top_func=self.top,
                                                                     phi_maker=self._make_phi_variable)
        self.stack_region.set_state(self)

        if register_region is not None:
            self.register_region: MultiValuedMemory = register_region
            self.register_region._phi_maker = self._make_phi_variable
        else:
            self.register_region: MultiValuedMemory = MultiValuedMemory(memory_id="reg", top_func=self.top,
                                                                        phi_maker=self._make_phi_variable)
        self.register_region.set_state(self)

        if global_region is not None:
            self.global_region: MultiValuedMemory = global_region
            self.global_region._phi_maker = self._make_phi_variable
        else:
            self.global_region: MultiValuedMemory = MultiValuedMemory(memory_id="mem", top_func=self.top,
                                                                      phi_maker=self._make_phi_variable)
        self.global_region.set_state(self)

        # Used during merging
        self.successor_block_addr: Optional[int] = None
        self.phi_variables: Dict[SimVariable,SimVariable] = {}

        self.typevars = TypeVariables() if typevars is None else typevars
        self.type_constraints = set() if type_constraints is None else type_constraints
        self.delayed_type_constraints = defaultdict(set) \
            if delayed_type_constraints is None else delayed_type_constraints

    def _get_weakref(self):
        return weakref.proxy(self)

    @staticmethod
    def top(bits) -> claripy.ast.BV:
        if bits in VariableRecoveryStateBase._tops:
            return VariableRecoveryStateBase._tops[bits]
        r = claripy.BVS("top", bits, explicit_name=True)
        VariableRecoveryStateBase._tops[bits] = r
        return r

    @staticmethod
    def is_top(thing) -> bool:
        if isinstance(thing, claripy.ast.BV) and thing.op == "BVS" and thing.args[0] == 'TOP':
            return True
        return False

    def extract_variables(self, expr: claripy.ast.Base) -> Generator[Tuple[int,Union[SimVariable,SpOffset]],None,None]:
        for anno in expr.annotations:
            if isinstance(anno, VariableAnnotation):
                yield from anno.addr_and_variables

    def annotate_with_variables(self, expr: claripy.ast.Base,
                                addr_and_variables: Iterable[Tuple[int,Union[SimVariable,SpOffset]]]) -> claripy.ast.Base:

        annotations_to_remove = [ ]
        for anno in expr.annotations:
            if isinstance(anno, VariableAnnotation):
                annotations_to_remove.append(anno)

        if annotations_to_remove:
            expr = expr.remove_annotations(annotations_to_remove)

        expr = expr.annotate(VariableAnnotation(list(addr_and_variables)))
        return expr

    def stack_address(self, offset: int) -> claripy.ast.Base:
        base = claripy.BVS("stack_base", self.arch.bits, explicit_name=True)
        if offset:
            return base + offset
        return base

    @staticmethod
    def is_stack_address(addr: claripy.ast.Base) -> bool:
        return "stack_base" in addr.variables

    @staticmethod
    def get_stack_offset(addr: claripy.ast.Base) -> Optional[int]:
        if "stack_base" in addr.variables:
            if addr.op == "BVS":
                return 0
            elif addr.op == "__add__":
                if len(addr.args) == 2 and addr.args[1].op == "BVV":
                    return addr.args[1]._model_concrete.value
                if len(addr.args) == 1:
                    return 0
            elif addr.op == "__sub__" and len(addr.args) == 2 and addr.args[1].op == "BVV":
                return -addr.args[1]._model_concrete.value
        return None

    def stack_addr_from_offset(self, offset: int) -> int:
        if self.arch.bits == 32:
            base = 0x7fff_fe00
            mask = 0xffff_ffff
        elif self.arch.bits == 64:
            base = 0x7f_ffff_fffe_0000
            mask = 0xffff_ffff_ffff_ffff
        else:
            raise RuntimeError("Unsupported bits %d" % self.arch.bits)
        return (offset + base) & mask

    @property
    def func_addr(self):
        return self.function.addr

    @property
    def dominance_frontiers(self):
        return self._analysis._dominance_frontiers

    @property
    def variable_manager(self):
        return self._analysis.variable_manager

    @property
    def variables(self):
        for ro in self.stack_region:
            for var in ro.internal_objects:
                yield var
        for ro in self.register_region:
            for var in ro.internal_objects:
                yield var

    def get_variable_definitions(self, block_addr):
        """
        Get variables that are defined at the specified block.

        :param int block_addr:  Address of the block.
        :return:                A set of variables.
        """

        return self._analysis.get_variable_definitions(block_addr)

    def add_type_constraint(self, constraint):
        """
        Add a new type constraint.

        :param constraint:
        :return:
        """

        self.type_constraints.add(constraint)

    #
    # Private methods
    #

    def _make_phi_variable(self, values: Set[claripy.ast.Base]) -> Optional[claripy.ast.Base]:
        # we only create a new phi variable if the there is at least one variable involved
        variables = set()
        bits: Optional[int] = None
        for v in values:
            bits = v.size()
            for _, var in self.extract_variables(v):
                variables.add(var)

        if len(variables) <= 1:
            return None

        assert self.successor_block_addr is not None

        # find existing phi variables
        phi_var = self.variable_manager[self.function.addr].make_phi_node(self.successor_block_addr, *variables)
        for var in variables:
            self.phi_variables[var] = phi_var

        r = self.top(bits)
        r = self.annotate_with_variables(r, [(0, phi_var)])
        return r

    # def _make_phi_variables(self, successor, state0, state1):
#
    #     stack_variables = defaultdict(set)
    #     register_variables = defaultdict(set)
#
    #     for state in [ state0, state1 ]:
    #         stack_vardefs = state.stack_region.get_all_variables()
    #         reg_vardefs = state.register_region.get_all_variables()
    #         for var in stack_vardefs:
    #             stack_variables[(var.offset, var.size)].add(var)
    #         for var in reg_vardefs:
    #             register_variables[(var.reg, var.size)].add(var)
#
    #     replacements = {}
#
    #     for variable_dict in [stack_variables, register_variables]:
    #         for _, variables in variable_dict.items():
    #             if len(variables) > 1:
    #                 # Create a new phi variable
    #                 phi_node = self.variable_manager[self.function.addr].make_phi_node(successor, *variables)
    #                 # Fill the replacements dict
    #                 for var in variables:
    #                     if var is not phi_node:
    #                         replacements[var] = phi_node
#
    #     return replacements

    def _phi_node_contains(self, phi_variable, variable):
        """
        Checks if `phi_variable` is a phi variable, and if it contains `variable` as a sub-variable.

        :param phi_variable:
        :param variable:
        :return:
        """

        if self.variable_manager[self.function.addr].is_phi_variable(phi_variable):
            return variable in self.variable_manager[self.function.addr].get_phi_subvariables(phi_variable)
        return False
