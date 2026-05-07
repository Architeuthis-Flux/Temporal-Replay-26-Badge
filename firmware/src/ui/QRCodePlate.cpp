#include "QRCodePlate.h"

#include "../hardware/oled.h"

namespace QRCodePlate {

void draw(oled& d, const uint8_t* bits, int width, int height,
          int x, int y, int plateSize, bool divider) {
  d.setDrawColor(1);
  d.drawBox(x, y, plateSize, plateSize);

  if (bits && width > 0 && height > 0) {
    int qrX = x + (plateSize - width) / 2;
    int qrY = y + (plateSize - height) / 2;
    if (qrX < x) qrX = x;
    if (qrY < y) qrY = y;

    d.setDrawColor(0);
    d.drawXBM(qrX, qrY, width, height, bits);
  }

  d.setDrawColor(1);
  if (divider) d.drawVLine(x + plateSize, y, plateSize);
}

}  // namespace QRCodePlate
