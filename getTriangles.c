_BYTE *__usercall physx::Gu::TriangleMesh::getTriangleMeshFlags@<X0>(
        physx::Gu::TriangleMesh *this@<X0>,
        _BYTE *a2@<X8>)
{
  char v2; // w1
  _BYTE *result; // x0

  v2 = *((_BYTE *)this + 92);
  result = a2;
  *a2 = v2;
  return result;
}


__int64 __fastcall physx::Gu::TriangleMesh::getTriangles(physx::Gu::TriangleMesh *this)
{
  return *((_QWORD *)this + 6);
}

__int64 __fastcall physx::Gu::TriangleMesh::getNbTriangles(physx::Gu::TriangleMesh *this)
{
  return *((unsigned int *)this + 8);
}