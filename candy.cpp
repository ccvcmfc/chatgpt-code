// Insert inside the pixel loop, immediately before calculating the average.
// Required variables:
// px, py, width, height: pixel position and image dimensions.
// xBegin, xEnd: half-open index range.
// sumX: array of column sums.
// sum: sliding sum for that range.
// count: number of samples used for the average.
// Requires <QDebug>.

if ((py == 0 || py == height / 2 || py == height - 1) &&
    (px == 0 || px == width / 2 || px == width - 1))
{
    double directSum = 0.0;

    for (auto x = xBegin; x < xEnd; ++x)
        directSum += sumX[x];

    QDebug debug = qDebug();
    debug.setRealNumberPrecision(17);
    debug.nospace()
        << "X check pixel=(" << px << "," << py << ")"
        << " range=[" << xBegin << "," << xEnd << ")"
        << " direct=" << directSum
        << " sliding=" << sum
        << " sumDiff=" << (sum - directSum)
        << " avgDiff=" << ((sum - directSum) / count)
        << " count=" << count;
}
