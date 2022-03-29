#include "Buffer.h"

Buffer::Buffer() {

}


bool Buffer::init() {
  bufA = (uint8_t*)EWH_MALLOC(BUF_SIZE);
  if ( bufA == NULL ) {
    log_e("Panic! Can't malloc %d bytes for buffer 1", BUF_SIZE );
    return false;
  }
  bufB = (uint8_t*)EWH_MALLOC(BUF_SIZE);
  if ( bufB == NULL ) {
    log_e("Panic! Can't malloc %d bytes for buffer 2", BUF_SIZE );
    return false;
  }
  return true;
}



void Buffer::checkFS(fs::FS* fs) {
  if ( !fs->exists( folderName ) ) {
    fs->mkdir( folderName );
  }
}


void Buffer::pruneZeroFiles(fs::FS* fs)
{
  fs::File root = fs->open( folderName );
  if ( ! root.isDirectory() ) {
    log_e("'%s' is not a directory", folderName );
    root.close();
    return;
  }
  fs::File fileToCheck;
  while ( fileToCheck = root.openNextFile() ) {
    if ( fileToCheck.isDirectory() ) continue;
#if defined ESP_IDF_VERSION_MAJOR && ESP_IDF_VERSION_MAJOR >= 4
    String path = fileToCheck.path();
#else
    String path = fileToCheck.name();
#endif
    size_t size = fileToCheck.size();
    fileToCheck.close();
    if ( path.endsWith(".log") && size == 0 ) {
      fs->remove( path.c_str() );
      Serial.printf("Removed empty file: %s\n", path.c_str() );
    } else {
      log_d("Found existing log file: %s (%d bytes)", path.c_str(), size );
    }
  }
  root.close();
}


bool Buffer::open(fs::FS* fs) {
  int i = 0;
  fileNameStr[0] = 0;

  do {
    sprintf( fileNameStr, fileNameTpl, folderName, i );
    i++;
    if ( i > 0xffff ) {
      log_e("Max files per folder exceeded, aborting !");
      return false;
    }
  } while (fs->exists( fileNameStr ));

  Serial.printf( "Current log file is: %s\n", fileNameStr );

  bufSizeA = 0;
  bufSizeB = 0;

  writing = true;

  useSD = true;
  return true;
}

void Buffer::close(fs::FS* fs) {
  if (!writing) return;
  forceSave(fs);
  writing = false;
  Serial.printf( "File: %s closed\n", fileNameStr );
}

uint64_t Buffer::micros64() {
  uint32_t micros_low = micros();
  if (micros_low < previous_micros)
    ++micros_high;
  previous_micros = micros_low;
  return (uint64_t(micros_high) << 32) + micros_low;
}

void Buffer::addBeacon(drone_beacon_t* beacon) {
  char buf[200];
  uint32_t len = (uint32_t)sprintf(buf, "%s,%s,%i,%i,%i,%i,%i,%i,%u,%u,%u,%u\n", beacon->id_fr, beacon->id_ansi, beacon->lat, beacon->lon,
                                   beacon->alt, beacon->height, beacon->lat_start, beacon->lon_start, beacon->speed,
                                   beacon->bearing, beacon->lastreceived_timestamp, beacon->firstreceived_timestamp);

  // buffer is full -> drop packet
  if ((useA && bufSizeA + len >= BUF_SIZE && bufSizeB > 0) || (!useA && bufSizeB + len >= BUF_SIZE && bufSizeA > 0)) {
    Serial.print(";");
    return;
  }

  if (useA && bufSizeA + len + 16 >= BUF_SIZE && bufSizeB == 0) {
    useA = false;
    Serial.println("\nswitched to buffer B");
  }
  else if (!useA && bufSizeB + len + 16 >= BUF_SIZE && bufSizeA == 0) {
    useA = true;
    Serial.println("\nswitched to buffer A");
  }

  write(buf, len); // packet payload
}

void Buffer::write(int32_t n) {
  uint8_t buf[4];
  buf[0] = n;
  buf[1] = n >> 8;
  buf[2] = n >> 16;
  buf[3] = n >> 24;
  write(buf, 4);
}

void Buffer::write(uint32_t n) {
  uint8_t buf[4];
  buf[0] = n;
  buf[1] = n >> 8;
  buf[2] = n >> 16;
  buf[3] = n >> 24;
  write(buf, 4);
}

void Buffer::write(uint16_t n) {
  uint8_t buf[2];
  buf[0] = n;
  buf[1] = n >> 8;
  write(buf, 2);
}

void Buffer::write(uint8_t n) {
  write(&n, 1);
}

void Buffer::write(uint8_t* buf, uint32_t len) {
  if (!writing) return;

  if (useA) {
    memcpy(&bufA[bufSizeA], buf, len);
    bufSizeA += len;
  } else {
    memcpy(&bufB[bufSizeB], buf, len);
    bufSizeB += len;
  }
}

void Buffer::write(char* buf, uint32_t len) {
  if (!writing) return;

  if (useA) {
    memcpy(&bufA[bufSizeA], buf, len);
    bufSizeA += len;
  } else {
    memcpy(&bufB[bufSizeB], buf, len);
    bufSizeB += len;
  }
}

void Buffer::save(fs::FS* fs) {
  if (saving) return; // makes sure the function isn't called simultaneously on different cores

  // buffers are already emptied, therefor saving is unecessary
  if ((useA && bufSizeB == 0) || (!useA && bufSizeA == 0)) {
    //Serial.printf("useA: %s, bufA %u, bufB %u\n",useA ? "true" : "false",bufSizeA,bufSizeB); // for debug porpuses
    return;
  }

  Serial.println("saving file");

  uint32_t startTime = millis();
  uint32_t finishTime;

  file = fs->open( fileNameStr, FILE_APPEND );
  if (!file) {
    Serial.printf("Failed to open file %s\n", fileNameStr );
    useSD = false;
    return;
  }

  saving = true;

  uint32_t len;

  if (useA) {
    file.write(bufB, bufSizeB);
    len = bufSizeB;
    bufSizeB = 0;
  }
  else {
    file.write(bufA, bufSizeA);
    len = bufSizeA;
    bufSizeA = 0;
  }

  file.close();

  finishTime = millis() - startTime;

  Serial.printf("\n%u bytes written for %u ms\n", len, finishTime);

  saving = false;

}

void Buffer::forceSave(fs::FS* fs) {
  uint32_t len = bufSizeA + bufSizeB;
  if (len == 0) return;

  file = fs->open(fileNameStr, FILE_APPEND);
  if (!file) {
    Serial.printf("Failed to open file %s\n", fileNameStr);
    useSD = false;
    return;
  }

  saving = true;
  writing = false;

  if (useA) {

    if (bufSizeB > 0) {
      file.write(bufB, bufSizeB);
      bufSizeB = 0;
    }

    if (bufSizeA > 0) {
      file.write(bufA, bufSizeA);
      bufSizeA = 0;
    }

  } else {

    if (bufSizeA > 0) {
      file.write(bufA, bufSizeA);
      bufSizeA = 0;
    }

    if (bufSizeB > 0) {
      file.write(bufB, bufSizeB);
      bufSizeB = 0;
    }

  }

  file.close();

  Serial.printf("saved %u bytes\n", len);

  saving = false;
  writing = true;
}
