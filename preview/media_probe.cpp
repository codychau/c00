// 电视墙模型层断言。重点是"解析失败要老实失败"：刮削器最糟糕的
// 失败模式不是解析不出来，而是解析错了却装作成功，把错误的标题
// 和年份写进用户的视野里。
#include "nasmodel.h"
#include <QCoreApplication>
#include <cstdio>

static int fail = 0;
static void check(bool c, const QString &w)
{
    printf("  %s  %s\n", c ? "OK " : "!! ", qPrintable(w));
    if (!c) ++fail;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    printf("=== 扩展名识别 ===\n");
    check(Nas::isVideoExtension("/a/b/x.mkv"), "mkv 是视频");
    check(Nas::isVideoExtension("/a/b/x.MKV"), "大小写不敏感");
    check(Nas::isVideoExtension("/a/b/game.iso"), "iso 算（蓝光原盘）");
    // .ts 刻意不算视频：不靠调用方记得过滤
    check(!Nas::isVideoExtension("/a/b/vite.config.ts"), ".ts 不靠约定，直接不认");
    check(!Nas::isVideoExtension("/a/b/seg.ts"), "小 .ts 也不算");
    // 完整判定才认 MPEG-TS
    check(Nas::isVideoFile("/a/b/seg.ts", 80LL * 1024 * 1024), "isVideoFile 认 80MB 的 .ts");
    check(!Nas::isVideoFile("/a/b/vite.config.ts", 4096), "isVideoFile 不认 4KB 的 .ts");
    check(Nas::isVideoFile("/a/b/x.mkv", 100), "isVideoFile 对非 .ts 不看体积");
    check(!Nas::isVideoExtension("/a/b/x.txt"), "txt 不是视频");
    check(!Nas::isVideoExtension("/a/b/noext"), "无扩展名不是视频");
    check(Nas::isSubtitleExtension("/a/x.zh.srt"), "srt 是字幕");

    printf("\n=== .ts 歧义：必须靠体积区分 ===\n");
    check(!Nas::looksLikeMpegTs("/p/vite.config.ts", 4096),
          "4KB 的 .ts 判为源码，不是视频");
    check(Nas::looksLikeMpegTs("/p/seg001.ts", 60LL * 1024 * 1024),
          "60MB 的 .ts 判为 MPEG-TS");
    check(Nas::looksLikeMpegTs("/p/x.mkv", 10), "非 .ts 不受这一关限制");

    printf("\n=== 文件名解析：电影 ===\n");
    {
        Nas::NameGuess g = Nas::guessFromFileName("Blade.Runner.2049.2017.2160p.HDR.x265.mkv");
        check(g.year == 2017, QString("年份取 2017 而不是 2049 -> %1").arg(g.year));
        check(!g.title.contains("2017"), QString("标题里不留年份 -> [%1]").arg(g.title));
    }
    {
        Nas::NameGuess g = Nas::guessFromFileName("Inception (2010).mkv");
        check(g.year == 2010, "括号年份");
        check(g.title == "Inception", QString("标题干净 -> [%1]").arg(g.title));
    }
    {
        // 这一条才真正锁住「括号年份」这条分支：年份后面紧跟数字时，
        // 裸年份正则的 (?![0-9]) 会拒绝匹配，只有括号分支能取到年份。
        // 早先没有这条，删掉 yearParen 整个分支测试都还是全绿的。
        Nas::NameGuess g = Nas::guessFromFileName("Some Movie (2019)2160p.mkv");
        check(g.year == 2019,
              QString("括号年份后紧跟数字仍能识别 -> %1（裸正则会漏）").arg(g.year));
    }

    printf("\n=== 文件名解析：剧集 ===\n");
    {
        Nas::NameGuess g = Nas::guessFromFileName("The.Expanse.S03E05.1080p.mkv");
        check(g.season == 3 && g.episode == 5,
              QString("S03E05 -> S%1E%2").arg(g.season).arg(g.episode));
        check(g.title == "The Expanse", QString("标题 -> [%1]").arg(g.title));
        check(!g.title.contains("S03"), "标题不含季集标记");
    }
    {
        Nas::NameGuess g = Nas::guessFromFileName("Firefly.2002.S01E01.mkv");
        check(g.year == 2002, QString("同时有年份与季集：年份 -> %1").arg(g.year));
        check(g.season == 1 && g.episode == 1, "季集仍解析");
        check(g.title == "Firefly", QString("标题在年份处截断 -> [%1]").arg(g.title));
    }
    {
        Nas::NameGuess g = Nas::guessFromFileName("Show.1x02.mkv");
        check(g.season == 1 && g.episode == 2, "1x02 记法");
    }
    {
        // 解析不出来必须返回空，不能瞎猜
        Nas::NameGuess g = Nas::guessFromFileName("IMG_20240115_112233.mp4");
        check(g.season == 0 && g.episode == 0, "无季集标记时不编造季集");
    }

    printf("\n=== NFO 解析：movie.nfo ===\n");
    {
        const QString xml = QString::fromUtf8(
            "<?xml version=\"1.0\" encoding=\"utf-8\" standalone=\"yes\" ?>\n"
            "<movie>\n"
            "  <title>Big Buck Bunny</title>\n"
            "  <originaltitle>bbb</originaltitle>\n"
            "  <sorttitle>Big Buck 01</sorttitle>\n"
            "  <originallang>en</originallang>\n"
            "  <premiered>2008-05-13</premiered>\n"
            "  <studio>Blender Foundation</studio>\n"
            "  <tagline><![CDATA[Funny & short film]]></tagline>\n"
            "  <plot>A rabbit &lt;hero&gt; takes &amp; revenge.</plot>\n"
            "  <genre>Animation</genre>\n"
            "  <genre>Comedy</genre>\n"
            "  <rating>8.1</rating>\n"
            "  <imdbid>tt1413194</imdbid>\n"
            "  <tmdbid>7301</tmdbid>\n"
            "  <actor><name>Blender Test</name><role>Himself</role></actor>\n"
            "</movie>\n");
        const Nas::NfoInfo n = Nas::parseNfo(xml);
        check(n.ok, "解析成功");
        check(n.title == "Big Buck Bunny", "title");
        check(n.originalTitle == "bbb", "originaltitle");
        check(n.sortTitle == "Big Buck 01", "sorttitle");
        check(n.year == 2008, QString("premiered 里的年份 -> %1").arg(n.year));
        check(n.studios == QStringList{"Blender Foundation"}, "studio");
        check(n.genres == (QStringList{"Animation","Comedy"}), "两个 genre");
        check(qFuzzyCompare(n.rating, 8.1), "rating");
        check(n.imdbId == "tt1413194", "imdbid");
        check(n.tmdbId == "7301", "tmdbid");
        check(n.tagline == "Funny & short film", QString("CDATA 取内容 -> [%1]").arg(n.tagline));
        check(n.plot.contains("<hero>"), QString("实体 &lt; 正确解码 -> [%1]").arg(n.plot));
        check(n.plot.contains("& revenge"), "amp 在 lt 之后解码，不被二次解错");
        check(n.actors == QStringList{"Blender Test"}, "actor name");
    }

    printf("\n=== NFO 解析：tvshow 与 episode ===\n");
    {
        const QString xml = "<tvshow><title>Sample Show</title>"
                            "<premiered>2015-01-01</premiered>"
                            "<episode>12</episode></tvshow>";
        const Nas::NfoInfo n = Nas::parseNfo(xml);
        check(n.ok && n.title == "Sample Show", "tvshow title");
        check(n.year == 2015, "tvshow 年份");
    }
    {
        const QString xml = "<episodedetails><title>Pilot</title>"
                            "<season>2</season><episode>7</episode></episodedetails>";
        const Nas::NfoInfo n = Nas::parseNfo(xml);
        check(n.season == 2 && n.episode == 7, "episode 季集");
    }

    printf("\n=== 解析失败必须老实失败（不许装作成功）===\n");
    {
        const Nas::NfoInfo n = Nas::parseNfo("");
        check(!n.ok, "空内容 -> 失败");
    }
    {
        const Nas::NfoInfo n = Nas::parseNfo("just some text, not xml");
        check(!n.ok, "非 XML -> 失败");
    }
    {
        // 有标签但没标题：不足以展示，必须算失败
        const Nas::NfoInfo n = Nas::parseNfo("<movie><imdbid>tt000</imdbid></movie>");
        check(!n.ok, "只有 id 没有标题 -> 失败，显示原始文件名而不是空卡片");
    }
    {
        // 未闭合标签：不能返回半截内容当成功
        const QString xml = "<movie><title>Truncated";
        const Nas::NfoInfo n = Nas::parseNfo(xml);
        check(!n.ok, "标签未闭合 -> 失败");
    }
    {
        const Nas::NfoInfo n = Nas::parseNfo("<movie><title>  </title></movie>");
        check(!n.ok, "标题全空白 -> 失败");
    }

    printf("\n=== TypeScript 类型声明不得当视频（实测踩过）===\n");
    // .d.mts / .d.cts / .d.ts 是 TS 类型声明。mts/cts 看着像 MPEG-TS，
    // 真实扫描把 index.d.mts 列成了影片。这类不能靠体积区分：
    // 类型声明可能几百 KB，真正的 .mts 分片也可能小于阈值。
    check(!Nas::isVideoExtension("/p/index.d.mts"), "index.d.mts 不是视频");
    check(!Nas::isVideoExtension("/p/index.d.cts"), "index.d.cts 不是视频");
    check(!Nas::isVideoExtension("/p/index.D.MTS"), "大小写不敏感");
    check(!Nas::isVideoFile("/p/index.d.mts", 500 * 1024), "isVideoFile 也挡（给足体积）");
    check(!Nas::isVideoFile("/p/index.d.ts", 90 * 1024 * 1024), "d.ts 即使很大也不算");
    check(Nas::isVideoExtension("/p/movie.mts"), "真 .mts 仍算视频");
    check(Nas::isVideoExtension("/a/DCS_TRUE.iso"), "iso 仍算（蓝光原盘）");
    check(Nas::isVideoFile("/a/DCS_TRUE.iso", 3158LL * 1024 * 1024), "大 iso 通过完整判定");

    printf("\n=== nfo 文件识别 ===\n");
    check(Nas::looksLikeNfoFile("/m/movie.nfo"), "movie.nfo");
    check(Nas::looksLikeNfoFile("/m/SHOW.NFO"), "大小写不敏感");
    check(!Nas::looksLikeNfoFile("/m/movie.mkv"), "mkv 不是 nfo");

    printf("\n%s (%d 失败)\n", fail ? "!! 有失败" : "OK 全部通过", fail);
    return fail ? 1 : 0;
}
