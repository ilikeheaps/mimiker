import gdb
import physmem
import utils
import traceback


class Kdump(gdb.Command, utils.OneArgAutoCompleteMixin):
    """ examine current kernel state

    Currently supported commands:
     * segments   - all memory segments
     * free_pages - free pages per segment
    """
    def __init__(self):
        super(Kdump, self).__init__("kdump", gdb.COMMAND_USER)
        # classes instead of functions in case we decide to store
        # information about structures in the debugger itself later on
        self.structure = {
            'segments': physmem.KernelSegments(),
            'free_pages': physmem.KernelFreePages(),
        }

    def invoke(self, args, from_tty):
        if len(args) < 1:
            raise(gdb.GdbError('Usage: kdump [structure]. Structures: {}.'
                               .format(self.structure.keys())))
        if args not in self.structure:
            raise gdb.GdbError('No such structure - {}.'.format(args))
        try:
            self.structure[args].invoke()
        except:
            traceback.print_exc()

    def options(self):
        return self.structure.keys()
