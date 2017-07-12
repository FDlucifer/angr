import angr

class __tls_get_addr(angr.SimProcedure):
    # pylint: disable=arguments-differ
    def run(self, ptr):
        module_id, offset = self.state.mem[ptr].uintptr_t.array(2).resolved
        if module_id.symbolic:
            raise SimValueError("__tls_get_addr called with symbolic module ID")
        module_id = self.state.any_int(module_id)

        return self.project.loader.tls_object.get_addr(module_id, offset)


# this is a rare and highly valuable TRIPLE-UNDERSCORE tls_get_addr. weird calling convention.
class ___tls_get_addr(angr.SimProcedure):
    # pylint: disable=arguments-differ
    def run(self):
        if self.state.arch.name == 'X86':
            ptr = self.state.regs.eax
            return self.inline_call(__tls_get_addr, ptr).ret_expr
        else:
            raise angr.errors.SimUnsupportedError("___tls_get_addr only implemented for x86. Talk to @rhelmot.")
