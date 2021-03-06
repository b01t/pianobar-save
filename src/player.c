/*
Copyright (c) 2008-2014
        Lars-Dominik Braun <lars@6xq.net>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

/* receive/play audio stream */

#include "config.h"

#include <arpa/inet.h>
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/avfiltergraph.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avio.h>
#include <libavutil/avutil.h>
#ifdef HAVE_LIBAVFILTER_AVCODEC_H
/* required by ffmpeg1.2 for avfilter_copy_buf_props */
#include <libavfilter/avcodec.h>
#endif
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>

#include "player.h"
#include "ui.h"
#include "ui_types.h"

/* default sample format */
const enum AVSampleFormat avformat = AV_SAMPLE_FMT_S16;

static void printError(const BarSettings_t *const settings,
                       const char *const msg, int ret) {
    char avmsg[128];
    av_strerror(ret, avmsg, sizeof(avmsg));
    BarUiMsg(settings, MSG_ERR, "%s (%s)\n", msg, avmsg);
}

/*	global initialization
 *
 *	XXX: in theory we can select the filters/formats we want to support, but
 *	this does not work in practise.
 */
void BarPlayerInit() {
    ao_initialize();
    av_log_set_level(AV_LOG_FATAL);
    av_register_all();
    avfilter_register_all();
    avformat_network_init();
}

void BarPlayerDestroy() {
    avformat_network_deinit();
    ao_shutdown();
}

/*	Update volume filter
 */
void BarPlayerSetVolume(player_t *const player) {
    assert(player != NULL);

    if (player->mode != PLAYER_PLAYING) {
        return;
    }

    int ret;
#ifdef HAVE_AVFILTER_GRAPH_SEND_COMMAND
    /* ffmpeg and libav disagree on the type of this option (string vs. double)
     * -> print to string and let them parse it again */
    char strbuf[16];
    snprintf(
        strbuf, sizeof(strbuf), "%fdB",
        player->settings->volume + (player->gain * player->settings->gainMul));
    assert(player->fgraph != NULL);
    if ((ret = avfilter_graph_send_command(player->fgraph, "volume", "volume",
                                           strbuf, NULL, 0, 0)) < 0) {
#else
    /* convert from decibel */
    const double volume = pow(10, (player->settings->volume +
                                   (player->gain * player->settings->gainMul)) /
                                      20);
    /* libav does not provide other means to set this right now. it might not
     * even work everywhere. */
    assert(player->fvolume != NULL);
    if ((ret = av_opt_set_double(player->fvolume->priv, "volume", volume, 0)) !=
        0) {
#endif
        printError(player->settings, "Cannot set volume", ret);
    }
}

#define softfail(msg)                       \
    printError(player->settings, msg, ret); \
    return false;

/*	ffmpeg callback for blocking functions, returns 1 to abort function
 */
static int intCb(void *const data) {
    player_t *const player = data;
    assert(player != NULL);
    if (player->interrupted > 1) {
        /* got a sigint multiple times, quit pianobar (handled by main.c). */
        player->doQuit = true;
        return 1;
    } else if (player->interrupted != 0) {
        /* the request is retried with the same player context */
        player->interrupted = 0;
        return 1;
    } else {
        return 0;
    }
}

static bool openStream(player_t *const player) {
    assert(player != NULL);
    /* no leak? */
    assert(player->fctx == NULL);

    int ret;

    /* stream setup */
    player->fctx = avformat_alloc_context();
    player->fctx->interrupt_callback.callback = intCb;
    player->fctx->interrupt_callback.opaque = player;

    assert(player->url != NULL);
    if ((ret = avformat_open_input(&player->fctx, player->url, NULL, NULL)) <
        0) {
        softfail("Unable to open audio file");
    }

    if ((ret = avformat_find_stream_info(player->fctx, NULL)) < 0) {
        softfail("find_stream_info");
    }

    /* ignore all streams, undone for audio stream below */
    for (size_t i = 0; i < player->fctx->nb_streams; i++) {
        player->fctx->streams[i]->discard = AVDISCARD_ALL;
    }

    player->streamIdx =
        av_find_best_stream(player->fctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (player->streamIdx < 0) {
        softfail("find_best_stream");
    }

    player->st = player->fctx->streams[player->streamIdx];
    player->st->discard = AVDISCARD_DEFAULT;

    /* decoder setup */
    if ((player->cctx = avcodec_alloc_context3(NULL)) == NULL) {
        softfail("avcodec_alloc_context3");
    }
    const AVCodecParameters *const cp = player->st->codecpar;
    if ((ret = avcodec_parameters_to_context(player->cctx, cp)) < 0) {
        softfail("avcodec_parameters_to_context");
    }

    AVCodec *const decoder = avcodec_find_decoder(cp->codec_id);
    if (decoder == NULL) {
        softfail("find_decoder");
    }

    if ((ret = avcodec_open2(player->cctx, decoder, NULL)) < 0) {
        softfail("codec_open2");
    }

    if (player->lastTimestamp > 0) {
        av_seek_frame(player->fctx, player->streamIdx, player->lastTimestamp,
                      0);
    }

    player->songPlayed = 0;
    player->songDuration =
        av_q2d(player->st->time_base) * (double)player->st->duration;

    char *save_dir = player->settings->save_dir;
    char tmp_filename[1000];
    char save_path[1000];
    char save_filename[1000];
    char save_complete[1000];
    int len_flag = 0;

    if (save_dir == NULL) {
        player->save_file = false;
    } else {
        player->save_file = true;
    }

    if (player->save_file) {
        char *music;
        int i;

        if (save_dir[strlen(save_dir) - 1] == '/') {
            strcpy(save_path, save_dir);
        } else {
            sprintf(save_path, "%s/", save_dir);
        }

        char artist[100], album[100];
        int offset = 0;
        for (i = 0; i < strlen(player->artist); ++i) {
            if (player->artist[i] == '"') {
                artist[i + offset] = '\\';
                artist[i + offset + 1] = player->artist[i];
                ++offset;
            } else if (player->artist[i] == '/') {
                artist[i + offset] = ' ';
            } else {
                artist[i + offset] = player->artist[i];
            }
        }
        artist[i + offset] = '\0';

        offset = 0;
        for (i = 0; i < strlen(player->album); ++i) {
            if (player->album[i] == '"') {
                album[i + offset] = '\\';
                album[i + offset + 1] = player->album[i];
                ++offset;
            } else if (player->album[i] == '/') {
                album[i + offset] = ' ';
            } else {
                album[i + offset] = player->album[i];
            }
        }
        album[i + offset] = '\0';

        sprintf(save_path, "%s%s/%s/", save_path, artist, album);

        struct stat st = {0};
        if (stat(save_path, &st) == -1) {
            char buf[1000];
            sprintf(buf, "mkdir -p \"%s\"", save_path);
            system(buf);
        }

        sprintf(save_filename, "%s.aac", player->title);

        for (int i = 0; i < 1000; i++) {
            if (save_filename[i] == '/') {
                save_filename[i] = ' ';
            } else if (save_filename[i] == '$') {
                save_filename[i] =
                    'S';  // Quick hack for errors from $* expansion
            }
        }

        if (stat("/tmp/pianobar", &st) == -1) {
            mkdir("/tmp/pianobar", 0700);
        }

        sprintf(tmp_filename, "/tmp/pianobar/%s", save_filename);
        strcpy(player->tmp_filename, tmp_filename);

        char jpgpath[500];
        sprintf(jpgpath, "%s/cover.jpg", save_path);

        CURL *easyhandle = curl_easy_init();

        curl_easy_setopt(easyhandle, CURLOPT_URL, player->album_art);

        FILE *file = fopen(jpgpath, "w");

        curl_easy_setopt(easyhandle, CURLOPT_WRITEDATA, file);
        curl_easy_perform(easyhandle);
        curl_easy_cleanup(easyhandle);

        fclose(file);

        sprintf(save_complete, "%s%s", save_path, save_filename);

        save_complete[strlen(save_complete) - 3] = 'm';
        save_complete[strlen(save_complete) - 2] = 'p';
        save_complete[strlen(save_complete) - 1] = '3';

        strcpy(player->save_complete, save_complete);

        if (access(save_complete, F_OK) != -1) {
            player->save_file = false;
        } else {
            player->save_file = true;
        }
    }

    AVFormatContext *ofcx;
    AVOutputFormat *ofmt;
    AVStream *ost;

    if (player->save_file) {
        ofmt = av_guess_format(NULL, tmp_filename, NULL);
        ofcx = avformat_alloc_context();
        ofcx->oformat = ofmt;
        avio_open2(&ofcx->pb, tmp_filename, AVIO_FLAG_WRITE, NULL, NULL);

        ost = avformat_new_stream(ofcx, NULL);
        avcodec_copy_context(ost->codec, player->st->codec);

        ost->time_base = player->st->time_base;
        ost->codec->time_base = ost->time_base;

        avformat_write_header(ofcx, NULL);
    }

    player->ofcx = ofcx;
    player->ost = ost;

    return true;
}

/*	setup filter chain
 */
static bool openFilter(player_t *const player) {
    /* filter setup */
    char strbuf[256];
    int ret = 0;
    AVCodecParameters *const cp = player->st->codecpar;

    if ((player->fgraph = avfilter_graph_alloc()) == NULL) {
        softfail("graph_alloc");
    }

    /* abuffer */
    AVRational time_base = player->st->time_base;

    snprintf(strbuf, sizeof(strbuf),
             "time_base=%d/"
             "%d:sample_rate=%d:sample_fmt=%s:channel_layout=0x%" PRIx64,
             time_base.num, time_base.den, cp->sample_rate,
             av_get_sample_fmt_name(player->cctx->sample_fmt),
             cp->channel_layout);
    if ((ret = avfilter_graph_create_filter(
             &player->fabuf, avfilter_get_by_name("abuffer"), NULL, strbuf,
             NULL, player->fgraph)) < 0) {
        softfail("create_filter abuffer");
    }

    /* volume */
    if ((ret = avfilter_graph_create_filter(
             &player->fvolume, avfilter_get_by_name("volume"), NULL, "0dB",
             NULL, player->fgraph)) < 0) {
        softfail("create_filter volume");
    }

    /* aformat: convert float samples into something more usable */
    AVFilterContext *fafmt = NULL;
    snprintf(strbuf, sizeof(strbuf), "sample_fmts=%s",
             av_get_sample_fmt_name(avformat));
    if ((ret = avfilter_graph_create_filter(
             &fafmt, avfilter_get_by_name("aformat"), NULL, strbuf, NULL,
             player->fgraph)) < 0) {
        softfail("create_filter aformat");
    }

    /* abuffersink */
    if ((ret = avfilter_graph_create_filter(
             &player->fbufsink, avfilter_get_by_name("abuffersink"), NULL, NULL,
             NULL, player->fgraph)) < 0) {
        softfail("create_filter abuffersink");
    }

    /* connect filter: abuffer -> volume -> aformat -> abuffersink */
    if (avfilter_link(player->fabuf, 0, player->fvolume, 0) != 0 ||
        avfilter_link(player->fvolume, 0, fafmt, 0) != 0 ||
        avfilter_link(fafmt, 0, player->fbufsink, 0) != 0) {
        softfail("filter_link");
    }

    if ((ret = avfilter_graph_config(player->fgraph, NULL)) < 0) {
        softfail("graph_config");
    }

    return true;
}

/*	setup libao
 */
static bool openDevice(player_t *const player) {
    const AVCodecParameters *const cp = player->st->codecpar;

    ao_sample_format aoFmt;
    memset(&aoFmt, 0, sizeof(aoFmt));
    aoFmt.bits = av_get_bytes_per_sample(avformat) * 8;
    assert(aoFmt.bits > 0);
    aoFmt.channels = cp->channels;
    aoFmt.rate = cp->sample_rate;
    aoFmt.byte_format = AO_FMT_NATIVE;

    int driver = ao_default_driver_id();
    if ((player->aoDev = ao_open_live(driver, &aoFmt, NULL)) == NULL) {
        BarUiMsg(player->settings, MSG_ERR, "Cannot open audio device.\n");
        return false;
    }

    return true;
}

/*	decode and play stream. returns 0 or av error code.
 */
static int play(player_t *const player) {
    assert(player != NULL);

    AVPacket pkt;
    AVCodecContext *const cctx = player->cctx;
    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;

    AVFrame *frame = NULL, *filteredFrame = NULL;
    frame = av_frame_alloc();
    assert(frame != NULL);
    filteredFrame = av_frame_alloc();
    assert(filteredFrame != NULL);

    enum { FILL, DRAIN, DONE } drainMode = FILL;
    int ret = 0;
    while (!player->doQuit && drainMode != DONE) {
        if (drainMode == FILL) {
            ret = av_read_frame(player->fctx, &pkt);
            if (ret == AVERROR_EOF) {
                /* enter drain mode */
                drainMode = DRAIN;
                avcodec_send_packet(cctx, NULL);
            } else if (pkt.stream_index != player->streamIdx) {
                /* unused packet */
                av_packet_unref(&pkt);
                continue;
            } else if (ret < 0) {
                /* error, abort */
                break;
            } else {
                /* fill buffer */
                avcodec_send_packet(cctx, &pkt);
            }
        }

        AVPacket pkt_write = pkt;

        player->pkt_write = pkt;

        if (player->save_file) {
            pkt_write.stream_index = player->ost->id;
            pkt_write.pts = av_rescale_q(
                pkt_write.pts, player->fctx->streams[0]->codec->time_base,
                player->ofcx->streams[0]->time_base);

            pkt_write.dts = av_rescale_q(
                pkt_write.dts, player->fctx->streams[0]->codec->time_base,
                player->ofcx->streams[0]->time_base);
            av_write_frame(player->ofcx, &pkt_write);
        }

        /* pausing */
        pthread_mutex_lock(&player->pauseMutex);
        if (player->doPause) {
            av_read_pause(player->fctx);
            do {
                pthread_cond_wait(&player->pauseCond, &player->pauseMutex);
            } while (player->doPause);
            av_read_play(player->fctx);
        }
        pthread_mutex_unlock(&player->pauseMutex);

        while (!player->doQuit) {
            ret = avcodec_receive_frame(cctx, frame);
            if (ret == AVERROR_EOF) {
                /* done draining */
                drainMode = DONE;
                break;
            } else if (ret != 0) {
                /* no more output */
                break;
            }

            /* XXX: suppresses warning from resample filter */
            if (frame->pts == (int64_t)AV_NOPTS_VALUE) {
                frame->pts = 0;
            }
            ret = av_buffersrc_write_frame(player->fabuf, frame);
            assert(ret >= 0);

            while (true) {
                if (av_buffersink_get_frame(player->fbufsink, filteredFrame) <
                    0) {
                    /* try again next frame */
                    break;
                }

                const int numChannels = av_get_channel_layout_nb_channels(
                    filteredFrame->channel_layout);
                const int bps = av_get_bytes_per_sample(filteredFrame->format);
                ao_play(player->aoDev, (char *)filteredFrame->data[0],
                        filteredFrame->nb_samples * numChannels * bps);

                av_frame_unref(filteredFrame);
            }
        }

        player->songPlayed = av_q2d(player->st->time_base) * (double)pkt.pts;
        player->lastTimestamp = pkt.pts;

        av_packet_unref(&pkt);
    }

    av_frame_free(&filteredFrame);
    av_frame_free(&frame);

    return ret;
}

static void finish(player_t *const player) {
    ao_close(player->aoDev);
    player->aoDev = NULL;
    if (player->fgraph != NULL) {
        avfilter_graph_free(&player->fgraph);
        player->fgraph = NULL;
    }
    if (player->cctx != NULL) {
        avcodec_close(player->cctx);
        player->cctx = NULL;
    }
    if (player->fctx != NULL) {
        avformat_close_input(&player->fctx);
    }
}

/*	player thread; for every song a new thread is started
 *	@param audioPlayer structure
 *	@return PLAYER_RET_*
 */
void *BarPlayerThread(void *data) {
    assert(data != NULL);

    player_t *const player = data;
    uintptr_t pret = PLAYER_RET_OK;

    bool retry;
    do {
        retry = false;
        if (openStream(player)) {
            if (openFilter(player) && openDevice(player)) {
                player->mode = PLAYER_PLAYING;
                BarPlayerSetVolume(player);
                retry =
                    play(player) == AVERROR_INVALIDDATA && !player->interrupted;
            } else {
                /* filter missing or audio device busy */
                pret = PLAYER_RET_HARDFAIL;
            }
        } else {
            /* stream not found */
            pret = PLAYER_RET_SOFTFAIL;
        }
        player->mode = PLAYER_WAITING;
        finish(player);
    } while (retry);

    player->mode = PLAYER_FINISHED;

    if (player->save_file && !player->doQuit) {
        av_write_trailer(player->ofcx);
        avformat_free_context(player->ofcx);
        avio_close(player->ofcx->pb);

        char cmd[2000], tmpmp3[1000];

        strcpy(tmpmp3, player->tmp_filename);
        tmpmp3[strlen(tmpmp3) - 3] = 'm';
        tmpmp3[strlen(tmpmp3) - 2] = 'p';
        tmpmp3[strlen(tmpmp3) - 1] = '3';

        char artist[100], album[100], title[100];
        int offset = 0, i;
        for (i = 0; i < strlen(player->artist); ++i) {
            if (player->artist[i] == '"') {
                artist[i + offset] = '\\';
                artist[i + offset + 1] = player->artist[i];
                ++offset;
            } else if (player->artist[i] == '/') {
                artist[i + offset] = ' ';
            } else {
                artist[i + offset] = player->artist[i];
            }
        }
        artist[i + offset] = '\0';

        offset = 0;
        for (i = 0; i < strlen(player->album); ++i) {
            if (player->album[i] == '"') {
                album[i + offset] = '\\';
                album[i + offset + 1] = player->album[i];
                ++offset;
            } else if (player->album[i] == '/') {
                album[i + offset] = ' ';
            } else {
                album[i + offset] = player->album[i];
            }
        }
        album[i + offset] = '\0';
        offset = 0;

        for (i = 0; i < strlen(player->title); ++i) {
            if (player->title[i] == '"') {
                title[i + offset] = '\\';
                title[i + offset + 1] = player->title[i];
                ++offset;
            } else if (player->title[i] == '/') {
                title[i + offset] = ' ';
            } else {
                title[i + offset] = player->title[i];
            }
        }
        title[i + offset] = '\0';

        sprintf(cmd, "ffmpeg -i \"%s\" -c:a libmp3lame -ac 2 -q:a 2 \"%s\"",
                player->tmp_filename, tmpmp3);
        system(cmd);

        sprintf(
            cmd,
            "lame  --vbr-new --preset standard --tt \"%s\" --ta \"%s\" --tl "
            "\"%s\" --add-id3v2 \"%s\" \"%s\"",
            player->title, artist, album, tmpmp3, player->save_complete);
        printf("%s\n", cmd);
        system(cmd);
    }

    return (void *)pret;
}
