// mslrun: compile a Metal source file at run time and run its test kernels.
// usage: mslrun tests.metal manifest.txt state_in.bin cb.bin out.bin
// manifest lines: <kernel> <threads> <state offset in words> ...
// Each kernel gets buffer 0 = the state at its offset, buffer 1 = cb.bin.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>

int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc < 6) { fprintf(stderr, "usage: mslrun src manifest state cb out\n"); return 2; }
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { fprintf(stderr, "no Metal device\n"); return 1; }
        NSString *src = [NSString stringWithContentsOfFile:@(argv[1]) encoding:NSUTF8StringEncoding error:nil];
        MTLCompileOptions *opt = [MTLCompileOptions new];
        if (@available(macOS 15.0, *)) opt.mathMode = MTLMathModeSafe; else opt.fastMathEnabled = NO;
        NSError *err = nil;
        NSDate *t0 = [NSDate date];
        id<MTLLibrary> lib = [dev newLibraryWithSource:src options:opt error:&err];
        printf("compile: %.2f s\n", -[t0 timeIntervalSinceNow]);
        if (!lib) { printf("COMPILE ERROR:\n%s\n", err.localizedDescription.UTF8String); return 1; }
        NSData *state = [NSData dataWithContentsOfFile:@(argv[3])];
        NSData *cb = [NSData dataWithContentsOfFile:@(argv[4])];
        id<MTLBuffer> sb = [dev newBufferWithBytes:state.bytes length:state.length options:MTLResourceStorageModeShared];
        id<MTLBuffer> cbb = [dev newBufferWithBytes:cb.bytes length:cb.length options:MTLResourceStorageModeShared];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        FILE *mf = fopen(argv[2], "r"); char name[128]; int nt; long off; char rest[256];
        int nk = 0;
        id<MTLCommandBuffer> cmd = [q commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cmd computeCommandEncoder];
        while (fscanf(mf, "%127s %d %ld", name, &nt, &off) == 3) {
            if (!fgets(rest, sizeof rest, mf)) rest[0] = 0;
            id<MTLFunction> fn = [lib newFunctionWithName:@(name)];
            if (!fn) { printf("missing %s\n", name); continue; }
            id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:fn error:&err];
            if (!ps) { printf("pipeline %s: %s\n", name, err.localizedDescription.UTF8String); continue; }
            [enc setComputePipelineState:ps];
            [enc setBuffer:sb offset:(NSUInteger)off * 4 atIndex:0];
            [enc setBuffer:cbb offset:0 atIndex:1];
            [enc dispatchThreads:MTLSizeMake((NSUInteger)nt, 1, 1) threadsPerThreadgroup:MTLSizeMake((NSUInteger)(nt < 64 ? nt : 64), 1, 1)];
            nk++;
        }
        fclose(mf);
        [enc endEncoding]; [cmd commit]; [cmd waitUntilCompleted];
        if (cmd.error) printf("GPU error: %s\n", cmd.error.localizedDescription.UTF8String);
        [[NSData dataWithBytes:sb.contents length:sb.length] writeToFile:@(argv[5]) atomically:NO];
        printf("ran %d kernels\n", nk);
    }
    return 0;
}
