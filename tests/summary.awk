# 장부(build/ledger dump)와 학생 결과 CSV를 합쳐 판정 기준을 계산한다
# 실행: awk -f tests/summary.awk 장부 결과.csv
# G1 전원이 5과목의 결과를 앎 · G3 먼저 왔는데 못 받은 짝 · G4 종류별 인기 과목 성공률 · 됐는데 모름

# 배열 a[1..n]을 작은 순서로 정렬하고 p 백분위를 초로 반환한다
function pct(a, n, p,    gap, i, j, x) {
    for (gap = int(n / 2); gap > 0; gap = int(gap / 2))
        for (i = gap + 1; i <= n; i++) {
            x = a[i]
            for (j = i; j > gap && a[j - gap] > x; j -= gap) a[j] = a[j - gap]
            a[j] = x
        }
    i = int(n * p + 0.999)
    return a[i < 1 ? 1 : i] / 1000
}

FNR == NR {                                     # course C seats S granted G : 학생...
    for (i = 8; i <= NF; i++) won[$2, $i] = 1
    granted[$2] = $6
    next
}
FNR == 1 { FS = ","; $0 = $0 }
{
    s = $1; kind[s] = $2; c = $4; r = $5
    if ($2 == "leave") next
    if (r != "?") { known[s]++; if ($7 > last[s]) last[s] = $7 }
    seen[s] = 1
    if (r == "o" && !((c, s) in won)) ghost++
    if (((c, s) in won) && r != "o") { if (r == "x") lied++; else lost++ }
    if (c >= 10 && r != "?") d[++others] = $7
    if (c < 10) {
        n[$2]++; ok[$2] += ((c, s) in won)
        if ($6 >= 0) { m[c]++; t[c, m[c]] = $6; w[c, m[c]] = ((c, s) in won) }
    }
}
END {
    for (s in seen) {
        people++
        if (known[s] < 5) continue
        v[++all] = last[s]
        f[kind[s], ++fn[kind[s]]] = last[s]
    }
    for (c in m) if (granted[c] > 0)            # 받은 사람보다 먼저 온 못 받은 사람의 짝
        for (i = 1; i <= m[c]; i++) if (w[c, i])
            for (j = 1; j <= m[c]; j++) if (!w[c, j]) { pairs++; inv += t[c, j] < t[c, i] }
    printf "G1 결과 다 앎 %d/%d명 (%.1f%%)  끝난 시각 p50 %.1f초 p99 %.1f초 마지막 %.1f초\n", all, people,
        100 * all / people, pct(v, all, 0.5), pct(v, all, 0.99), pct(v, all, 1)
    printf "G3 먼저 왔는데 못 받은 짝 %.1f%%  됐는데 모름 %d  됐는데 마감이라 들음 %d  장부에 없는 성공 %d\n",
        pairs ? 100 * inv / pairs : 0, lost, lied, ghost
    split("calm press macro tabs cut", K, " ")
    printf "G4 인기 과목을 받은 비율"
    for (i = 1; i <= 5; i++) if (n[K[i]]) printf "  %s %.1f%%(%d)", K[i], 100 * ok[K[i]] / n[K[i]], n[K[i]]
    printf "\n끝난 시각 p50"
    for (i = 1; i <= 5; i++) {
        split("", one)
        for (j = 1; j <= fn[K[i]]; j++) one[j] = f[K[i], j]
        if (fn[K[i]]) printf "  %s %.1f초", K[i], pct(one, fn[K[i]], 0.5)
    }
    printf "  보통 과목의 답 p50 %.1f초 p99 %.1f초\n", pct(d, others, 0.5), pct(d, others, 0.99)
}
