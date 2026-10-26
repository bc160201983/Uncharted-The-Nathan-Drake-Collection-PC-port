#include "../gpu/shim/videodec2/videodec2.h"
#include "../gpu/shim/videodec2/videodec_error.h"
extern "C" {
#include <libavcodec/avcodec.h>
}
#include <cassert>
#include <cstdio>
#include <cstring>
#include <array>
#include "test_environment.h"
using namespace Libraries::Videodec2;
int main() {
    SetTestEnv("GOW3_MOVIE_SIZE", nullptr);
    OrbisVideodec2ComputeMemoryInfo compute{};
    compute.this_size=sizeof(compute);
    assert(!sceVideodec2QueryComputeMemoryInfo(&compute) && compute.cpu_gpu_memory_size);
    OrbisVideodec2DecoderConfigInfo config{};
    config.this_size=sizeof(config); config.codec_type=OrbisVideodec2CodecType::Avc;
    config.max_frame_width=64; config.max_frame_height=48; config.max_dpb_frame_count=4;
    OrbisVideodec2DecoderMemoryInfo memory{};
    memory.this_size=sizeof(memory);
    assert(!sceVideodec2QueryDecoderMemoryInfo(&config,&memory));
    assert(memory.max_frame_buffer_size>=64*48*3/2+sizeof(OrbisVideodec2AvcPictureInfo));
    OrbisVideodec2Decoder decoder=nullptr;
    assert(!sceVideodec2CreateDecoder(&config,&memory,&decoder));
    std::array<u8,65536> pixels{},stream{};
    FILE* file=fopen("tests/data/tiny.h264","rb"); assert(file);
    const int size=(int)fread(stream.data(),1,stream.size(),file); fclose(file);
    OrbisVideodec2FrameBuffer buffer{};
    buffer.this_size=sizeof(buffer); buffer.frame_buffer=pixels.data(); buffer.frame_buffer_size=pixels.size();
    OrbisVideodec2OutputInfo output{};
    output.this_size=48; /* Old SDK ABI must leave newer trailing fields untouched. */
    output.frame_format=0xabcdef01; output.frame_pitch_in_bytes=0x12345678;
    AVCodecParserContext* parser=av_parser_init(AV_CODEC_ID_H264); assert(parser);
    AVCodecContext* parsing=avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_H264)); assert(parsing);
    int units=0,pictures=0,seen=0;
    auto check=[&] {
        assert(output.frame_format==0xabcdef01 && output.frame_pitch_in_bytes==0x12345678);
        if (!output.is_valid) return;
        assert(output.frame_width==64 && output.frame_pitch==64 && output.frame_height==48);
        assert(output.frame_buffer_size==64*48*3/2 && buffer.is_accepted);
        OrbisVideodec2AvcPictureInfo info{}; info.this_size=sizeof(info);
        assert(!sceVideodec2GetPictureInfo(&output,&info,nullptr));
        assert(info.is_valid && info.attached_data>=1000 && info.attached_data<1006);
        int bit=1<<(info.attached_data-1000); assert(!(seen&bit)); seen|=bit; ++pictures;
        info.this_size=7;
        assert(sceVideodec2GetPictureInfo(&output,&info,nullptr)==ORBIS_VIDEODEC2_ERROR_STRUCT_SIZE);
    };
    const u8* next=stream.data();
    for (int left=size;;) {
        uint8_t* au=nullptr; int au_size=0; const bool flushing=!left;
        int used=av_parser_parse2(parser,parsing,&au,&au_size,next,left,AV_NOPTS_VALUE,AV_NOPTS_VALUE,0);
        assert(used>=0); next+=used; left-=used;
        if (au_size) {
            OrbisVideodec2InputData input{sizeof(input),au,(u64)au_size,(u64)units,(u64)units,1000+(u64)units};
            assert(!sceVideodec2Decode(decoder,&input,&buffer,&output)); check(); ++units;
        }
        if (flushing && !au_size) break;
    }
    for (int i=0;i<8;++i) { assert(!sceVideodec2Flush(decoder,&buffer,&output)); check(); }
    assert(units==6 && pictures==6 && seen==0x3f);
    av_parser_close(parser); avcodec_free_context(&parsing);
    assert(!sceVideodec2Reset(decoder));
    output.this_size=8;
    OrbisVideodec2InputData invalid{sizeof(invalid),stream.data(),(u64)size,0,0,0};
    assert(sceVideodec2Decode(decoder,&invalid,&buffer,&output)==ORBIS_VIDEODEC2_ERROR_STRUCT_SIZE);
    output.this_size=sizeof(output);
    invalid.au_size=0;
    assert(sceVideodec2Decode(decoder,&invalid,&buffer,&output)==ORBIS_VIDEODEC2_ERROR_ACCESS_UNIT_SIZE);
    assert(!sceVideodec2DeleteDecoder(decoder));
    puts("Videodec2: six decoded frames, reordered metadata and old output ABI tests passed");
}
