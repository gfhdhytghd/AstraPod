const std = @import("std");
const asm_mod = @import("asm");
const image = @import("image");

fn load(ctx: ?*anyopaque, gpa: std.mem.Allocator, path: []const u8) anyerror!?[]u8 {
    const io: *std.Io = @ptrCast(@alignCast(ctx.?));
    return std.Io.Dir.cwd().readFileAlloc(io.*, path, gpa, .limited(2 * 1024 * 1024)) catch |err| {
        if (err == error.FileNotFound) return null;
        return err;
    };
}

pub fn main(init: std.process.Init) !void {
    const gpa = init.gpa;
    var io = init.io;
    const args = try init.minimal.args.toSlice(init.arena.allocator());
    if (args.len != 3) return error.Arguments;
    const src = (try load(&io, gpa, args[1])) orelse return error.FileNotFound;
    defer gpa.free(src);
    var prog = switch (try asm_mod.assembleProgram(gpa, src,
        .{ .path = args[1], .loader = .{ .ctx = &io, .load = load } })) {
        .ok => |p| p,
        .err => |fe| {
            std.debug.print("assembly error {any}\n", .{fe});
            return error.Assemble;
        },
    };
    defer prog.deinit();
    const bytes = try image.write(gpa, &prog);
    defer gpa.free(bytes);
    try std.Io.Dir.cwd().writeFile(io, .{ .sub_path = args[2], .data = bytes });
}
